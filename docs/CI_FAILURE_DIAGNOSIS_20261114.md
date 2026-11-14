# Diagnosis of the first hosted full-suite failures (2026-11-14)

Independently checked by a fresh QA agent; its corrections are applied.

Scope: the "Diagnose first" slice cody chose on 2026-11-13. Read-and-test only. **No fix is in this
PR**; each proposed fix below is a separate slice that waits for review. Nothing in `bench/results/`,
the frozen campaign files or any preregistered outcome is touched.

The failures being explained are from the first hosted full run on `main` (run 43375591294, 12e7ec2):
47 of 49 tests passed; `test_baseline_move` (q10_sealed, q10_committed) and `test_xnode` failed.

Evidence came from throwaway draft PR #124 (branch `diag-ci-failures`, never to be merged), which reran
the failing tests on hosted runners with extra logging, and from local runs (4-vCPU container, kernel
6.18, same compiler 13.3.0 and libraries). Run IDs are listed at the end.

Labels used: **Observed** (read from a log or counter), **Inferred** (follows from observations,
not shown directly), **Open** (not yet explained).

## Summary

| Failure | What it is | Real defect? | Proposed next slice |
|---|---|---|---|
| q10 balance mismatch | After its standby times out, a primary shard stops serving (95 s in one run; to the end of the run in two) and never asks the witnesses to continue alone; 16 client requests go unanswered; the checker then counts them as wrong balances | Yes, very likely: a liveness defect (HA path). The checker also mislabels in-doubt transfers | F1 (server), F2 (checker) |
| test_xnode X16 | Test race: ENDs owed to a live member that the Out connection has moved away from stay owed, as the design says, but the test then expects them settled | Very likely a test race (Inferred; the end state is the one the design describes) | F3 (test; settle failure only) |
| test_xnode X3 | 32 items held at a node for its peer, on a bound link, never answered in 80 s | Likely a real liveness defect in the cross-node links | F4 (diagnosis first) |
| test_xnode X26 | 32 requests on a wire the test deliberately holds and drops are never resolved; usually they are | Open | F4 |

**Inferred:** no balance is corrupted. The net-zero mismatches match the unanswered transfers exactly
in count; the nonzero-net ones come with follow-ups still pending. No per-account reconciliation was
done, so this is not proven.

## 1. q10 (`tests/test_baseline_move.py`)

### Where it fails

| Where | q10 result | Client failovers per run |
|---|---|---|
| Local, 38 runs (16 pinned to 2 CPUs) | 38 / 38 pass | 4 |
| Hosted Intel Xeon 6975P-C runners (AWS kernel 7.0), 12 runs | 12 / 12 pass | 4 |
| Hosted AMD EPYC 9V45 runner (Azure kernel 7.0), round 2, 12 runs | 3 fail | 4 when passing; 57, 58, 66 when failing |
| Hosted Azure runners, CPU model not recorded: main (2 runs) and round 1 (8 runs) | main 2 fail; round 1 1 fail (a whole-runner stall, below) | main 44 and 64 (both failing); round 1 4 to 38 |

**Observed.** Client failovers are 44 to 66 in every failing run and 4 to 38 in passing ones: the
client keeps failing over while its requests go unanswered. Each failing run in round 2 starts with a
primary logging
`standby shard 0 stopped acking: treating it as down` (the harness sets `--replica-timeout-ms 300`).
The O_DSYNC-write and clock-gap probes in round 2 (one sample every 0.5 s) saw no write over 0.2 s and
no clock gap over 2.5 s for the whole job, so there was no long disk or host stall. Shorter hiccups
(around 0.3 to 2 s) are not ruled out. Why the standby missed its deadline is **Open**. Every q10
failure seen was on an Azure-hosted runner (each log's `Cloud:` line); the all-pass round 3 was AWS.

(One round 1 failure was different: all four nodes' in-progress checkpoints reported 1572.7 to 1573.0
s, and the load client's log stops at 26.1 s. **Inferred:** a whole-runner stall of about 26 minutes,
environmental. It is still counted in the table.)

### The stall

**Observed** (round 2, q10_sealed slot 1326; node 3 is the destination group's primary):

1. `snc: standby shard 0 stopped acking: treating it as down`
2. `snc: shard 0: blocked for T_block (no standby acknowledging), leaving the node links`
3. after the drop: no `witness: granted, continuing without the standby`, and no NOTREADY in any
   witness log (the last ones are the start-up records at t = 0.48 to 3.76 s)
4. `snc: shard 0: unblocked, rejoining the node links as a new instance`
5. `checkpoint: 305 objects in 94.80s`

The load client's throughput drops to 0 at about 18 s and stays at 0 to the end. Both client threads
end with all 8 requests in flight unanswered: `requests unanswered 60 s after the end: 8 in flight`.

The other two round 2 failures (q10_sealed slot 3000, q10_committed slot 3000; node 1 is the stalled
primary) show the same first two lines and then **nothing more from that node for the rest of the
run**: no witness grant, and no further `checkpoint:` line, although it had logged them regularly until
then (`--checkpoint-s 0.3`). **Inferred:** its checkpoint was still running when the harness stopped it.

Locally, 11 forced standby pauses (SIGSTOP, 0.3 to 2.3 s, during load) gave 10 detected drops, and
every one was followed by `witness: granted, continuing without the standby`. On the runners, node logs
exist only for failing runs. Round 1's failing run also shows several drops followed by checkpoints and
no grant line, so not every runner drop is followed by a grant; that is not yet explained.

The 94.80 s run logs `unblocked, rejoining the node links` before its checkpoint ends, while the
client's throughput stays 0; that unblock is not yet explained either (**Open**).

**Inferred (not yet proven).** The DEGRADE request to the witnesses is made only by `snc_server`'s main
loop (`tools/snc_server.cpp`, the `!granted && st.blocked > 0` branch). The same loop runs checkpoints
synchronously (`eng.checkpoint()`), and the checkpoint that started near the drop took 94.8 s. While the
main loop is inside that checkpoint, nobody can ask the witnesses, so the shard stays blocked. The
missing `checkpoint:` lines in the other two failures fit this: their checkpoints never finished. What holds
the checkpoint while the shard is blocked is **Open**. `docs/BASELINE_READINESS.md` B1 already removed
one such wait (the shard part waiting on the standby), so this may be a second path of the same shape.

### Why it shows as a balance mismatch

**Observed** (`bench/snc_load.cpp`): when the client gives up ("requests unanswered 60 s after the
end"), the transfers still in flight are not written to `PREFIX.unknown`. The checker therefore treats
them as not applied.

**Observed numbers.** In each failure both client threads end with 8 unanswered, so 16 transfers.
- main q10_sealed: 32 accounts off, net 0. Round 2 q10_sealed (slot 1326): 31 accounts off, net 0.
  **Inferred:** a transfer conserves money and touches two accounts, so 16 applied but unanswered
  transfers give 32 accounts off with net 0 (31 when two of them share an account).
- Nonzero net, with follow-ups still pending at the end: main q10_committed (−43, 2 pending), round 2
  q10_sealed slot 3000 (−128, 9 pending), round 2 q10_committed (−153, 6 pending). **Inferred:**
  credits not yet applied.

### Proposed fixes (each a separate slice, for review)

- **F1 (server):** find what holds `Engine::checkpoint()` while a shard is blocked, and make the
  DEGRADE request independent of the checkpoint (for example, ask the witnesses from a thread that a
  checkpoint cannot hold). Acceptance:
  - a deterministic test that blocks a primary's standby during a checkpoint and requires a witness
    grant within the normal bound;
  - q10 passing repeatedly on the runner class where it fails (AMD, round 2).
- **F2 (checker):** `snc_load` writes in-flight and to-retry transfers to `.unknown` when it gives up,
  so the check reports them as in doubt, not as corruption. The run still fails (rc 1, unanswered
  requests), so nothing is weakened. Acceptance:
  - main's q10_sealed numbers (16 unanswered) check as "16 in doubt";
  - an injected real mismatch still fails.

## 2. test_xnode (`tests/test_xnode.cpp`)

### Where it fails

| Where | Full `test_xnode` runs | Failing checks |
|---|---|---|
| Local | 5 / 20 failed | X16 settle (4), X16 "ENDs owed to member 0" wait (1) |
| Local, X16 alone | 2 / 55 failed | X16 settle |
| Hosted Intel runner | 4 / 8 failed | X26 lost requests, X16 settle, X16 ENDs wait, X3 held items |
| Hosted Azure runner, CPU model not recorded (round 1; main failed the same check, in X4) | 8 / 8 failed | lost requests (`:499`) in X3 (7) or X4 (1) |
| Hosted AMD EPYC 7763 runner (round 2, X3 X4 only) | 0 / 8 failed | none |

### X16: a test race against the documented design

**Observed** (local, entry dump at the failure): node 2 (B) holds, for group 0 member 1, `end_owed 64`
and `sync_owed 1`, while B's Out connection is bound to member 0's new incarnation. Member 1 holds
`count 64` in quarantine, so its entry no longer gates.

**Design** (`docs/CROSS_NODE_EXCHANGE_DESIGN.md`, X16 row): "member 2's debt is sent only on a binding
to member 2". The test moves B's Out connection from member 1 back to member 0 without first waiting
for member 1's items to end. **Inferred:** when some are still live at that moment, their ENDs stay owed and member 1
stays live, so nothing ever sends them; `settle_counts` then times out on `end_owed 64`. This is the
state the design describes, reached by a timing window the test does not close.

- **F3 (test):** before the second switch, wait until B owes member 1 nothing (or rebind to member 1
  once at the end so its debt is sent). The check is not loosened: the same end state (all counts 0)
  is required. Acceptance: X16 alone 0 failures in 200 local runs; the full test passes repeatedly
  locally and hosted.

### X3: items held on a bound link, never answered

**Observed** (hosted Intel runner, round 6, X3 — the first test — `Load 1->0`):
- 32 outstanding (a full window), after `ok 14088 overloaded 573598`.
- Node 0: `xnode_held_for_peers=32`, `xnode_links_bound=1`. Node 1: `xnode_remote_live=32`,
  `xnode_links_bound=1`. No quarantine on either side.
- All unchanged after a further 60 s wait.

So node 0 holds 32 of node 1's items on a bound link and never answers or ends them. Nothing in the
design lets an item be held indefinitely on a bound link. **Inferred:** a real liveness defect.

The round 1 X3/X4 losses (Azure runner, 8 of 8) happened before this logging existed, so whether they
are the same mechanism is **Open**.

### X26: deliberately dropped requests not resolved

**Observed** (hosted Intel runner, round 1, X26, `Load 0->2`): `ok 32`, 32 outstanding. Node 0 shows
`xnode_replies_returned=32` (the 32 answered). Node 2 shows `xnode_quarantines=1`,
`xnode_q_in_down=1` and `xnode_links_unbound=1`, all unchanged after a further 60 s.

The 32 outstanding are the full window that the test itself holds on member m0's wire and then cuts
("items on member m0's wire, never delivered"). The test also leaves `refuse(true)` set on that path, so
node 2's In side from node 0 cannot come back. Its staying unbound is what design 2.8 predicts (a
quarantine ends only by a binding over a live In connection). The test's comment "and reconnect" is
not met only because the test keeps the path refused.

The test passes most runs, so those 32 requests are normally resolved some other way, and in this run
they were not. **Open:** what normally resolves them, and whether a request lost on a cut wire must
always be answered (if not, this `Load` should be `may_lose`).

### X16 "ENDs owed to member 0" wait

This wait timed out once locally and once hosted. It is not yet diagnosed (**Open**).

- **F4 (diagnosis, then a fix slice):**
  - reproduce the X3 hold locally and find why the held items are never answered;
  - answer the X26 question above;
  - diagnose the X16 wait timeout.

  Each needs review before any code change.

## What this does not change

- CI stays as merged in #122: no test is skipped, retried or relabelled, and the `full` job stays red
  until these are fixed.
- D1 to D4 are not started.
- The diagnostic code (gauge dumps, samplers, probes) lives only on `diag-ci-failures` (PR #124), which
  will be closed unmerged.

## Evidence

- Main full run: GitHub Actions run 43375591294 (job 131721226153).
- PR #124 round 1 (3589d0d): run 43414473661. q10 job 131848765286; test_xnode job 131848765819.
- PR #124 round 2 (509094a): run 43555070716. q10 with probes: job 132325967424 (AMD EPYC, Azure).
- PR #124 round 3 (a861314): run 43557113094. Full test_xnode with gauge dumps: job 132332357696
  (Intel); q10: job 132332357914 (Intel, 12 of 12 pass).
- PR #124 round 2 test_xnode X3 X4 only: job 132325967201 (AMD EPYC 7763, Azure), 8 of 8 pass.
- Local: q10 38 of 38; standby SIGSTOP experiment (11 pauses of 0.3 to 2.3 s during load, 10 drops
  detected, all granted), pass; full test_xnode 5 of 20 failed; X16 alone 2 of 55 failed (entry dump
  above). Only the failing logs were kept, so the totals come from the loop's own counts.
- Not part of this diagnosis: a local `ctest -j4` run also failed `test_baseline_retry`
  `q5_catchup_race` with a port collision (PR #122 evidence, `docs/CI.md`). CI runs serially.
