# dc1 Track B harness slice 1: breaker review

One reviewer (Opus), told to break the `net_bench` change. The report follows verbatim. Disposition:
- Findings 1 to 11 are fixed. Ops are now classed as applied, not applied or in doubt. A resent deposit answered
  `E_REPLAYED_OLD` counts as applied; an in-doubt transfer widens that account's allowed range. The old `verify:`
  line prefix is kept. The probe SLO plus grace must be under `--drain-s`. Probe reads count toward errors and
  refusals. Only `E_APP` removes a probe. Probe creation retries `E_OVERLOADED` and fails cleanly. A probe account is
  reused only after both replies. Every (thread, shard) pair must have a probe account. `rejected` and probe reads
  count measured ops only.
- Finding 12: the stall is widened to 500 ms, and the conserve SLO is 1 s.
- UNRESOLVED 1 is settled by design: a credit is applied once per (source, seq) (`docs/TRANSFERS.md`), so once two
  scans match, no later duplicate can land.
- UNRESOLVED 2 (no deadline in the inner verify poll) predates the change and is left as it is.

After the fixes, `test_net_bench_verify`, `test_net_bench_rate`, `test_baseline_retry`, `test_sync_readiness`,
`test_index_wire` and `test_gauges_admin` pass locally.

## Review of the net_bench Track B diff (bench/net_bench.cpp, tests/test_net_bench_verify.py)

I built the change and the original in /tmp scratch copies. The new test passes (all 5 scenarios). An existing slow test now fails, so this change is not ready to merge. Line numbers below are in the modified bench/net_bench.cpp. Scratch dirs are deleted.

### BLOCKING

**1. `--verify` with `--session` now fails runs that passed before (reproduced).**
- Where: `on_run`, delta update at ~L261 (`st == Status::OK` only), and the probe decrement at ~L256.
- The old code counted deposits when sent. The new code counts only replies with status OK.
- A resend of an older sequence gets `E_REPLAYED_OLD`, which means the request was already applied. It is now left out of `delta`, so the account reads one higher than `want`.
- Failing scenario: `tests/test_baseline_retry.py` (ctest, label slow).
  - New binary: 10 of 13 scenarios FAIL. Examples:
    - q1_kill: "account 9 has 1827, expected 1826 … mismatches 34 … FAILED"
    - q5_kill_stop: 123 mismatches
  - Original binary on the same tree: q1_kill PASS, "mismatches 0".
  - `tests/test_sync_readiness.py` uses the same `--session --verify` deposits flow, so it should fail the same way. I did not run it.
- Smallest fix:
  - Count `E_REPLAYED_OLD` deposits as +1. A deposit cannot be refused, so "decided" means applied.
  - For transfers, `E_REPLAYED_OLD` (and any error reply after a resend) is in doubt. Keep a per-account in-doubt count and accept `want ≤ bal ≤ want + in_doubt` (credit side). This is the plan's "in-doubt as a bound".
  - Do not decrement `probe_expected` on `E_REPLAYED_OLD`.

**2. The verify output format changed, and three parsers still expect the old one.**
- Where: ~L686.
- Old line: `verify: N accounts, D deposits, M mismatches`
- New line: `verify: N accounts  scans S  mismatches M  sum … expected …: ok`
- Parsers that use the old regex:
  - `tests/test_baseline_retry.py:126`
  - `tests/test_sync_readiness.py:107`
  - `bench/run_retries.py:65`
- Each of these requires `deposits > 0`, so they fail even when verify passes.
- Fix: keep the old line's prefix, e.g. `verify: %zu accounts, %llu deposits, %llu mismatches  scans …  sum …`, with deposits = acknowledged deposits. Or update all three parsers in this change.

### MAJOR

**3. Probe reads still pending after the drain limit fail `--verify` and mark the run undrained, though no workload op is in doubt (reproduced).**
- Where: termination condition at ~L539, `undrained`, and verify at ~L611.
- Repro: `--rate 2000 --seconds 1 --probe-pct 5 --probe-slo-us 3000000 --drain-s 1 --verify` prints:
  - `drained no  remaining_unissued 0  remaining_in_flight 0`
  - `verify: run not drained, some ops in doubt: FAILED`
- Pilot would then report "not drained: 0 unissued, 0 in flight", which contradicts itself, and the run counts as not sustained.
- Fix (either):
  - Have `drained` and verify's precondition ignore pending probe reads (track `workload_outstanding` separately), and keep counting those probes as misses.
  - Or reject `probe_slo + grace >= drain_s` in `parse()`.

**4. Probe reads are left out of errors and refusals, against the plan.**
- Where: `probe_reply`, ~L224.
- The plan (4.1, "How the overhead is charged") says probe reads "count toward errors and toward the client-limit check".
- Here a probe read that returns E_OVERLOADED, E_STALE or another error only becomes a miss. It never reaches `errors` or `overloaded`, so pilot.py's "no errors, nothing refused" gates cannot see them.
- Fix: in `probe_reply`, bump `t->overloaded`, `t->stale` or `t->errors` by status, the same way as for ops.

**5. A probed transfer refused with E_OVERLOADED or E_STALE drops out of the probe count entirely.**
- Where: ~L253 (`st != OK` → `r.rejected = true`).
- It counts as neither probed nor skipped. Under overload the shed probes vanish, which helps the system under test and leaves the budget/skip check blind.
- The plan's rule is that a probe shows no credit → miss. Only E_APP (the transfer was refused, nothing to settle) is defensible to exclude.
- Fix: set `rejected` only for `E_APP`. Leave other non-OK replies as probes, so they miss, or count them as `skipped`.

**6. If `create_probes` fails, the process aborts instead of exiting 1.**
- Where: ~L895.
- `return 1` runs while every worker is still blocked at `ready.arrive_and_wait()`, so the joinable `std::thread`s are destroyed and `std::terminate` is called.
- `create_probes` also treats E_OVERLOADED on CREATE as fatal (load() retries it). The resulting error text is empty (`probe accounts: `), because `c.error()` is only set for transport errors.
- I did not reproduce the abort: the server's `--objects` limit did not refuse the creates. This finding comes from reading the code.
- Fix:
  - Follow the load-failure path: `sh.phase = kDrain; ready.arrive_and_wait(); join`.
  - Retry E_OVERLOADED.
  - Print the status.

### MINOR

**7. A late E_APP on a reused probe account can cause a false miss.**
- Where: `probe_reply` and ~L256.
- The account goes back to the free list when its read replies, even if the transfer reply is still pending.
- Scenario:
  1. Probe A on p (want S+1) misses because the transfer is slow.
  2. p is freed; probe B on p is created with want S+2.
  3. A's transfer then returns E_APP, so `probe_expected` drops back to S+1.
  4. B's read sees S+1, below its want of S+2, and counts as a miss.
- A also stays counted as a probe, because `rejected` is set after the read was counted.
- It only errs towards more misses and needs E_APP on the source, so it is rare with the 10^12 start balances.
- Fix: free p only once both replies are in, e.g. a 2-bit pending flag on the record.

**8. With sessions, a probe can pass without its own credit.**
- The `E_REPLAYED_OLD` decrement (ties to finding 1) lowers `want` for later probes on p, even though that credit was applied.
- A later probe whose own credit is missing can then pass. This is the only wrong-hit path I found.

**9. The probe-account check is too weak.**
- `probe_accounts >= threads` does not guarantee a non-empty bucket for each (thread, shard) pair.
- Repro: 4 threads, 2 shards, `--probe-accounts 4` gives `probed 31 … skipped 164`.
- It is reported, but the check could require `probe_accounts >= threads * shards`.

**10. The `rejected` percentage can exceed 100%.**
- Where: ~L975.
- `rejected` counts warm-up and drain replies, but the denominator `ops` counts measured replies only.
- Fix: count `rejected` only when `measured`, or divide by `served_all`.

**11. The probe `reads` count includes warm-up reads, but `probed` and `skipped` cover the window only.**
- The plan's check is "reads within 5% of B against offered ops". Document which denominator the ratio uses, or count reads only for measured probes.

**12. The new test is timing-dependent.**
- In `tests/test_net_bench_verify.py`, "late" relies on a 300 ms SIGSTOP and a ≥5% miss rate. "conserve" asserts 0 misses at a 1 s SLO.
- Both could flake on a loaded CI runner. Consider labelling the test slow or widening the margins.

### Checked and found sound
- Delta indexing after the zipf reorder: `by_owner` is now built from indexes, after the reorder.
- Transfers to self: delta nets to 0.
- `xfer-local` with probes: d is the destination shard, so locality is kept.
- n=1 shard: d=0.
- Multiple threads: per-thread deltas are summed, and probe accounts are partitioned by `p % threads`.
- `to_j = n_ids + p` matches verify's account order.
- Slot state reset on every reply path.
- Probe reads stay out of ops, latency, `in_window`, `served_all` and lateness.
- The probe FIFO is monotonic, because records are pushed in intended order.
- Closed loop: probes need `--rate`, which is enforced.
- Parsers in pilot.py, s6.py, test_pilot.py and test_net_bench_rate.py: the inserted `  rejected` line and the `probe:` line do not break their regexes.

### UNRESOLVED
- Verify uses two scans 50 ms apart. A later duplicate (retried) credit could land after both scans. The plan asks for `emits_now` = 0 and empty outboxes in two quiet rounds; I could not find the outbox retry period to judge whether 50 ms covers it.
- The verify inner `while (v.got < end)` has no deadline. This predates the change, but the loop now repeats for up to 60 s.

### Style
- The code is slim and comments are few.
- `Probe::done` exists only for the unread count, and `Probe::shard` repeats `owner_of(probe_ids[p])`. Both are acceptable.
- No speculative abstraction.

Unrelated: during the review `git status` began showing `docs/DIRECTION_COMPARISON_PLAN.md` modified and an untracked `docs/research/TRACKB_PREREQUISITES.md`. Both are outside the diff under review; I did not touch them.
