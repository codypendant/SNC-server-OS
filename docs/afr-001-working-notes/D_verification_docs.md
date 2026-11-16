# Recon D: verification estate, observability/operations, governing documents

Reviewer D, Phase 1 (reconnaissance and inventory). Read-only. Repo `/home/user/snc-server-os` at
HEAD `24006ee` ("Merge PR #135: D2-a default shard count ...", `git log -5`).

Labels: **VERIFIED** (I read the cited line or ran the command), **DOCUMENTED** (a repo doc says so;
I did not re-check it in code or by running it), **INFERRED** (follows from what I read; not shown
directly), **UNKNOWN**.

Commands run (all read-only): `ctest -N`, `ctest -N -L slow` in the scratchpad build; one run of the
five quick tests `test_pilot test_s6 test_profile_check test_ha_caller_trace_{on,off}` (all passed,
1.4 s); greps listed inline; `git log` and `git rev-parse` (read-only); `pdftotext` on one handoff PDF.

---

## 1. Test inventory

### 1.1 Registration facts

- 61 tests are registered (VERIFIED: `ctest -N` gives "Total Tests: 61"). 10 carry the label `slow`
  (VERIFIED: `ctest -N -L slow` lists test_xnode, test_health, test_health_baseline, test_fail_closed,
  test_s1, test_baseline_retry, test_sync_readiness, test_baseline_move, test_baseline_ha,
  test_ready_order; `CMakeLists.txt:170-171`). That leaves 51 in the fast tier.
- There is no separate list variable for C++ tests. The 28 C++ tests are an inline `foreach` list,
  `CMakeLists.txt:61-65`. Each is built from `tests/<name>.cpp`, linked to `snc_core` and registered
  with no arguments. VERIFIED.
- The 8 extra registrations are: 2 env variants (`CMakeLists.txt:67,69`), 4 conntrack-guard variants of
  `test_migrate` (`:75-87`, one marked `WILL_FAIL`, two matched by `PASS_REGULAR_EXPRESSION`), and
  `test_ha_caller` run twice (`:106-110`, built without snc_core). VERIFIED.
- 25 Python tests are registered only `if(PYTHON3)` (`:111-172`). VERIFIED.
- `TIMEOUT 2400` is set on test_s3_writeback, test_lifecycle, test_s4_sync_tokens, test_intake and
  test_xnode (`:90`). test_default_shards treats exit 77 as a skip (`:126`). VERIFIED.
- `tests/fixtures/conntrack/` holds only `full` and `room`. The `absent` variant (`:84-85`) relies on the
  directory not existing. VERIFIED (`ls`).
- `test_reader_placement` (STATUS.md:374, CPU_PLACEMENT_R1.md:107) is not a ctest test. It is a function
  inside `test_replication` (`tests/test_replication.cpp:531,1519`). VERIFIED.
- Scenarios that run by name only are never registered, so CI never runs them: test_baseline_retry
  q1_blocked, q4_delayed*, q3_runtime20, and test_xnode_proc x9, which needs `--old`
  (`CMakeLists.txt:129-143`; CI.md:75-76). VERIFIED.
- Shared harness code:
  - `tests/harness.h` (175 lines): run ops on shard threads.
  - `tests/check.h` (15 lines): the CHECK macro.
  - `tests/link_proxy.h` (409 lines): the fault proxy, used only by test_xnode.
  - `tests/test_health.py`: Python helpers such as `free_block` and `sanitizer()` at :308-313, imported
    as `th` by the other suites.
  - `tests/witness_state.py`.

  VERIFIED by reading the file heads and by grep counts.

### 1.2 Table

Key to the columns:
- **Lang.** C = C++. Py = Python driving built binaries.
- **Slow.** S = labelled slow.
- **Evidence kinds.**
  - U: unit or pure logic.
  - IP: in-process integration (Engine and shards in one process).
  - MP: multi-process (real `snc_server`, `snc_witness` or forked nodes).
  - K: SIGKILL or crash.
  - ST: SIGSTOP.
  - FI: in-process fault injection (`Engine::debug_*`, LinkProxy, `DEBUG` admin verbs).
  - T: timing bound asserted.
  - D: death or abort test.
- **Deps.**
  - io_uring: every test that links `snc_core` needs it (CMakeLists.txt:14,43), and CI.md:82-85
    requires it unprivileged.
  - ports: loopback TCP.
  - Py: built binaries from the build dir.

The kinds come from each file's header comment plus grep counts of fork/subprocess, SIGKILL, SIGSTOP,
LinkProxy, `DEBUG ` and `debug_*(` per file. They are VERIFIED as signals in the file. How strongly
each test checks what it claims is not assessed (INFERRED from headers).

| # | Test | Lang | Slow | Domain | Evidence | Notable deps / notes |
|---|---|---|---|---|---|---|
| 1 | test_rate_schedule | C | | bench: net_bench open-loop schedule | U | none; links snc_core |
| 2 | test_object_id | C | | object model: ObjectID | U | |
| 3 | test_local_map | C | | local map | U | |
| 4 | test_pages | C | | huge-page memory, arena spill (HUGEPAGES.md) | U | huge pages optional |
| 5 | test_runtime | C | | native runtime ring mesh: FIFO, no loss or dup, overflow | IP | pinned threads |
| 6 | test_execute | C | | execute(): ops, rejections, conditional, large args | IP | |
| 7 | test_persistence | C | | WAL, checkpoint, recovery, remap, torn tail, mint leases | IP | data dir; O_DIRECT path |
| 8 | test_crash | C | | durability under process kill | MP, K | fork + SIGKILL, 4 rounds |
| 9 | test_net | C | | TCP front end, routing, forwarding, malformed frame | IP | ports |
| 10 | test_transfer | C | | two-object transfers, conservation, SIGKILL | IP, K | fork |
| 11 | test_replication | C | | WAL shipping, sync/async, promotion, catch-up, reader placement | MP, K, ST, FI | 23 fork/kill sites; ports |
| 12 | test_cluster | C | | multi-group slot map, kill/restart, group failover | MP, K | ports |
| 13 | test_migrate | C | | slot moves under crashes (67 scenarios; FAILURE_MODEL.md:57) | MP, K | conntrack-guarded (TEST_MIGRATE_CONNECTION_CHURN.md); lanes from CPU count; ports |
| 14 | test_resend_refusals | C | | invariant 18: resend before refusal | IP | |
| 15 | test_proof | C | | placement-v2 serving proof | IP | ports |
| 16 | test_placement | C | | placement v1/v2 text form | U | |
| 17 | test_stage_chain | C | | S1 bounded destination staging chain | IP, FI | |
| 18 | test_gauges | C | | S0 published gauges (refusal causes, peaks, WAL mode) | IP | reports actual O_DIRECT or buffered mode |
| 19 | test_s1_admission | C | | S1 resource admission | IP, FI | 32 debug-hook sites |
| 20 | test_s2_pacing | C | | S2 checkpoint walk controller | U (+ seeded fuzz of bounds, :383), IP | |
| 21 | test_s3_writeback | C | | S3 bounded checkpoint writeback; short-write and -EIO injection | IP, FI, D | TIMEOUT 2400 |
| 22 | test_lifecycle | C | | S3.2 engine lifecycle gate | IP, FI, D | TIMEOUT 2400 |
| 23 | test_s4_sync_tokens | C | | S4 K sync tokens; fsync -EIO injection | IP, FI, D | TIMEOUT 2400 |
| 24 | test_admin_loop | C | | S4.1 admin loop, replies whole or cut; K0 keep | IP, T | ports |
| 25 | test_wal_capacity | C | | WalStream growth arithmetic, fail-stop | U, D | |
| 26 | test_intake | C | | D.1 follow-up intake; F10 contract death tests | IP, FI, D | TIMEOUT 2400; "load-slow, not shown flaky" (STATUS.md:325) |
| 27 | test_xnode | C | S | D.2 cross-node exchange accounting, X1-X35 | IP, FI (LinkProxy cut/hold/stall/drop, `LinksGeneration::debug_crash_after`) | **flaky** (section 1.3); 168 sleep/deadline sites; TIMEOUT 2400 |
| 28 | test_io_yield | C | | F1 regression: shard loop blocks with I/O in flight | IP | reads `/proc` voluntary ctxt switches; pipe held write |
| 29 | test_admin_loop_keep_off | C | | as 24 with SNC_HA_KEEP=0 | IP | env |
| 30 | test_migrate_keep_off | C | | one scenario (copied/src-restart) with keep off | MP, K | |
| 31 | test_migrate_conntrack_refuses | C | | guard refuses an oversubscribed conntrack table | config check | WILL_FAIL; fixture |
| 32 | test_migrate_conntrack_says_why | C | | guard message text | config check | regex on output |
| 33 | test_migrate_conntrack_room | C | | guard allows with room | MP | |
| 34 | test_migrate_conntrack_absent | C | | guard with no conntrack files | MP | |
| 35 | test_ha_caller_trace_off | C | | ha::Caller trace labels, tracing off | U | SNC_TRACE unset |
| 36 | test_ha_caller_trace_on | C | | same, tracing on | U | SNC_TRACE=1 |
| 37 | test_witness | Py | | replicated witnesses: grant and promote never both | MP, K, ST, FI | fake primary and standby; many ports |
| 38 | test_async_failover | Py | | R08 characterization: async promote loses acked writes | MP, K, ST | |
| 39 | test_control | Py | | control plane end to end | MP, K | |
| 40 | test_health | Py | S | HEALTH decoupled from shard work (DEBUG STALL) | MP, K, ST, FI, T | source of the `free_block` port helper |
| 41 | test_health_baseline | Py | S | HEALTH/failover detection baseline (kill, stop, partition, accept stall, EMFILE) | MP, K, ST, FI, T | bounds asserted unless SNC_SANITIZED (:50-52); prlimit |
| 42 | test_gauges_admin | Py | | GAUGES/HEALTH admin side under net_bench load | MP | --debug-admin |
| 43 | test_ckpt_cli | Py | | snc_server walk flags parsing | U-ish (process start) | |
| 44 | test_default_shards | Py | | default shard count from the affinity set | MP | taskset/affinity; skip 77 |
| 45 | test_fail_closed | Py | S | fail-closed migration serving (F1-F18 scenarios) | MP, K, FI (DEBUG REPL-PAUSE) | 45 kill sites; `--old` cases not registered |
| 46 | test_s1 | Py | S | S1 bounded staging with real nodes | MP, K, FI (DEBUG MOVE-STOP) | |
| 47 | test_profile_check | Py | | bench/profile_check.py refusal rules | U | no build used |
| 48 | test_baseline_retry | Py | S | T-Q1/T-Q5 retries across failover | MP, K, ST, FI | port collision under -j4 (CI.md:33-38) |
| 49 | test_sync_readiness | Py | S | B2 promotion eligibility | MP, K | |
| 50 | test_baseline_move | Py | S | T-Q2/T-Q10 moves under HA | MP, K, FI | q10 was red on hosted runners until F1 (section 1.3) |
| 51 | test_xnode_proc | Py | | D.2 with processes (x6, x7, x7b, x33) | MP, K, ST | x9 needs `--old`, not registered |
| 52 | test_baseline_ha | Py | S | T-Q3/T-Q4 Profile B | MP, K, ST | |
| 53 | test_ready_order | Py | S | causal order of NOT READY/READY | MP, K, ST, FI | |
| 54 | test_ckpt_failover | Py | | a running checkpoint does not hold failover (DEBUG CKPT-HOLD) | MP, K, ST, FI, T | `GRANT_BOUND_S = 20` (:53) |
| 55 | test_paxos_ballots | Py | | F8 Paxos ballots after witness restart | MP, K, ST, FI | witness `--debug 1` |
| 56 | test_witness_hardening | Py | | witness state-file hardening | MP | hand-written state files |
| 57 | test_lab_witness_guard | Py | | G2 campaign guard in bench/lab/lab.py on loopback | MP | runs lab.py |
| 58 | test_index_wire | Py | | INDEX wire kind (X1 of EXPERIMENTAL_BASELINE) | MP, ST | |
| 59 | test_pilot | Py | | bench/pilot.py rules, CP-1A freeze interlock | U | needs full git history (ci.yml:35); **passes now** (I ran it) |
| 60 | test_s6 | Py | | bench/s6.py rules | U | no processes |
| 61 | test_net_bench_rate | Py | | net_bench open-loop --rate and --mix | MP, T | asserts latency and lateness ≥ thresholds (:115-139); CPU-sensitive (CI.md:29-32) |

Other dependencies:
- No test checks for root (`grep geteuid|root` in tests: no hits; VERIFIED).
- The lab's iptables and netem faults need root, but only on real hosts (`bench/lab/lab.py:18-19,421-432`).
- O_DIRECT is the WAL default. The `/tmp` filesystem decides whether it is honored; test_gauges T3
  reports the mode actually used (`tests/test_gauges.cpp` header). DOCUMENTED and INFERRED.
- Requirements stated in `docs/CI.md:82-85`: `RWF_DSYNC`, writable `/tmp` and `/dev/shm`, 4 vCPUs and
  Python 3.
- Some Python tests key on server log text. test_ckpt_failover matches
  `"witness: granted, continuing without the standby"` (`tests/test_ckpt_failover.py:50`). VERIFIED.
  Others match startup lines (INFERRED from grep of 10 files).

### 1.3 Known red or flaky tests (latest documented state)

- **test_pilot.**
  - It was deterministically red, a stale assertion against `go_record`
    (ARCHITECTURE_EVIDENCE_AUDIT_20261113.md:84,425).
  - It was corrected in PR #122 (STATUS.md:330-331).
  - VERIFIED it passes at HEAD (I ran it: Passed 1.14 s).
- **test_xnode.**
  - It is flaky. CI_FAILURE_DIAGNOSIS_20261114.md:123-129 records 5/20 local failures (X16 settle and
    the X16 END wait) and 4/8 on hosted Intel (X26, X16, X3), all DOCUMENTED. It classifies X16 as a
    test race (F3) and X3/X26 as likely or open liveness defects (F4).
  - The fixes are not started: "F2 to F4 not started" (STATUS.md:391), DOCUMENTED.
    `tests/test_xnode.cpp` was last changed at 2bccdbe, before the diagnosis (VERIFIED: `git log -- tests/test_xnode.cpp`).
  - **INFERRED: still flaky at HEAD.** No later doc records a hosted full run with test_xnode passing.
    The K0 doc's 60/60 local full suite (K0_TEST_PROBE.md:160-163) is one local pass and does not
    contradict flakiness.
- **test_baseline_move q10.**
  - It was red on Azure-hosted runners (CI_FAILURE_DIAGNOSIS:36-46).
  - Its root cause was CPU starvation, fixed by F1/PR #126 (io_yield): "Hosted q10 20/100 failed before,
    0/200 after" (STATUS.md:335-339). DOCUMENTED.
  - The checker mislabel (F2, `bench/snc_load.cpp` not writing in-flight transfers to `.unknown`) is
    unfixed (STATUS.md:391). DOCUMENTED.
- **test_migrate.**
  - Its hosted failures were fixed by harness placement (PR #127) and the conntrack guard (PR #128)
    (STATUS.md:346-355).
  - It also failed `copied/src-restart` on a hosted run and on a main nightly (CHECKPOINT_FAILOVER.md:90-93).
    The resolution is in TEST_MIGRATE_ROOT_CAUSE.md (CPU placement), DOCUMENTED.
  - Current hosted state: UNKNOWN.
- **test_baseline_retry q5_catchup_race.**
  - It hit a port collision under `-j4` (CI.md:33-38).
  - It is mitigated only by running serially. DOCUMENTED.
- **test_net_bench_rate.**
  - It failed once at `-j4` from CPU contention (CI.md:29-32).
- **Latest whole-suite records.**
  - K0: 60/60 local, `-j1`, before D2-a added test_default_shards (K0_TEST_PROBE.md:160-163).
  - CHECKPOINT_FAILOVER: hosted 50/51 (CHECKPOINT_FAILOVER.md:90).
  - No record of a hosted full run at HEAD with 61 tests: UNKNOWN.

---

## 2. Fault injection and verification capabilities

### 2.1 Present (VERIFIED unless marked)

| Capability | Where | Used by |
|---|---|---|
| Process kill (SIGKILL) and restart | fork in C++ tests; `subprocess` + kill in Python | test_crash, test_transfer, test_replication, test_cluster, test_migrate, most Py suites |
| Process stop (SIGSTOP/SIGCONT) | Python suites | test_health*, test_baseline_*, test_ready_order, test_async_failover, test_witness, test_xnode_proc |
| Link fault proxy: PASS/HOLD/DROP/STALL per direction, cut, refuse, retarget, replay, frame inject | `tests/link_proxy.h:1-13,81-141` | test_xnode only (52 references) |
| Checkpoint chunk write fault: one byte short or -EIO | `Engine::debug_ckpt_fail_write` (`include/snc/snc.h:681-691`; `src/core/shard.cpp:42,2455-2462`) | test_s3_writeback :633,:1057 |
| Checkpoint final fdatasync -EIO | `Engine::debug_ckpt_fail_sync` (`snc.h:700-705`; `shard.cpp:53,2533`) | test_s4_sync_tokens :778 |
| Checkpoint holds and orderings | `debug_ckpt_hold_writes/defer_writes/release_writes/hold_commit/hold_superblock/hold_sync/release_syncs/walk_limit` (`snc.h:674-705`) | S2, S3, S4 tests |
| WAL flush hold (no flush; not an error injection) | `debug_hold_wal` (`snc.h:671-673`) | S1 admission |
| Replication pause | `debug_pause_replication` (`snc.h:668-670`) | test_replication |
| Lifecycle parking | `debug_park_ops/park_start/park_promote/promote_at_control` (`snc.h:706-715`) | test_lifecycle |
| Migration stage stop and crash | `debug_stage_point/stage_stop` (engine.cpp) | test_stage_chain |
| Links-generation crash points | `LinksGeneration::debug_crash_after` (`src/core/links_generation.h:60`) | test_xnode :1415 |
| Client drop of answers | `debug_drop_answers` (`include/snc/net.h`) | |
| Admin `DEBUG` verbs, only with `--debug-admin` | `tools/snc_server.cpp`: `DEBUG STALL` :1106, `ACCEPT-HOLD` :1131, `REPL-PAUSE` :1152, `DROP-ANSWERS` :1162, `CKPT-HOLD commit/sync` :1174-1180, `MOVE-STOP <phase> <unit> hold:MS\|crash` :1187-1204 | Py suites |
| Witness `DEBUG` verb | `tools/snc_witness.cpp:517` (`debug_verb`) | test_paxos_ballots, test_witness* |
| Descriptor exhaustion (EMFILE via prlimit) | test_health_baseline.py:29-31 | |
| conntrack-table fixtures | `tests/fixtures/conntrack/` | test_migrate_conntrack_* |
| Host network faults: iptables partition and one-way, NIC down, netem delay (root, real hosts) | `bench/lab/lab.py:18-19,421-432` | lab runs, not CI |
| Tracing `SNC_TRACE=1` (stderr) | `include/snc/trace.h:2,26,56`; 36 call sites (grep `SNC_TRACE(`) | diagnosis; test_ha_caller |
| Consistency checker: money conservation plus single ownership | `bench/snc_load.cpp` `check`; `AUDIT` verb (`tools/snc_server.cpp:202-228,1277`) | lab, Py suites. EVIDENCE.md:33: "No history checker beyond money conservation and single ownership" |
| Seeded fuzz of one pure controller's bounds | `tests/test_s2_pacing.cpp:11,383` | only "fuzz" in tests |
| Sanitizer-aware harness | `tests/test_health.py:308-313` reads `-fsanitize` from CMakeCache; looser bounds | |

### 2.2 Absent (evidence of absence)

| Capability | Search | Result |
|---|---|---|
| WAL write/fsync I/O error injection | `grep -rn "EIO\|ENOSPC\|LD_PRELOAD\|failpoint" src tests include tools` | EIO appears only in the checkpoint paths (`shard.cpp:2455-2533`), `storage.cpp:817` (`storage_fatal`, abort) and `paxos.h:497`. No WAL injection. VERIFIED. FAILURE_MODEL S3 is UNPROVEN (DOCUMENTED). |
| Power-loss harness (dm-flakey, dm-log-writes, LazyFS, VM cut) | `grep -rniE "power.?loss\|dm-flakey\|LazyFS\|crashmonkey"` | only statements that it is untested: STATUS.md:596 "power loss is not tested"; FAILURE_MODEL H3/S5 UNPROVEN; audit QA_REPORT.md:39,128. VERIFIED absent. |
| Media-corruption tests (mid-segment bit flip, superblock, manifest) | same grep; audit D2 | none registered. Audit D2 reproduced silent loss manually (ARCHITECTURE_EVIDENCE_AUDIT:49). `torn_tails` is never exported by snc_server: it appears only in `src/core/engine.cpp:1105,1167` (VERIFIED grep of tools/src). |
| Model checking (TLA+, P, Alloy) | `grep -rliE "TLA\+\|tlaplus\|\.tla\b\|alloy"` | only the audit docs, as a recommendation. No spec files. VERIFIED. |
| Linearizability or history checker (Jepsen, Knossos, Porcupine, Elle) | `grep -rliE "lineariz\|jepsen\|knossos\|porcupine"` | "linearizable" appears only for the placement read (`engine.cpp:953,996,1340`, `control.h:67`). No checker. VERIFIED. |
| Deterministic simulation (controlled time, network, disk) | grep | EVIDENCE.md:33: "No controlled time, network delivery or storage completion; no deterministic simulation". research/README.md:110 lists FoundationDB as a reference. VERIFIED absent. |
| Fuzzing (libFuzzer, AFL) of wire, WAL or parsers | `grep -rliE "libfuzzer\|LLVMFuzzerTestOneInput\|afl"` | none. The only hit is the seeded controller fuzz. VERIFIED. |
| Sanitizer CI | `grep fsanitize CMakeLists.txt .github` | none; README.md:28-29 gives manual commands only. CI.md:88-91 lists sanitizers as "Not yet in CI". VERIFIED. Manual runs exist, for example TSan 47/47 at 2bccdbe in `bench/results/xnode-d2-review-fixes-devvm-20261110/README.md:15-16`. |
| Packet loss, reorder, jitter, flapping | FAILURE_MODEL N5-N7 | UNPROVEN (DOCUMENTED, FAILURE_MODEL.md:69-80) |
| Coverage measurement | `grep -rn "coverage\|gcov\|lcov" CMakeLists.txt .github` | none (INFERRED; no target or flag) |

---

## 3. CI

The workflow is `.github/workflows/ci.yml` (79 lines). VERIFIED.

- **Triggers:**
  - `pull_request` (any base);
  - `push` to `main`;
  - `schedule` cron `17 7 * * *`;
  - `workflow_dispatch` (:12-18).
- **Concurrency:** cancels in-progress runs only for PRs (:23-25).
- **Job `fast`:**
  - Runs on every trigger, on `ubuntu-24.04`, 30 min limit.
  - Checks out with `fetch-depth: 0` for test_pilot.
  - Installs `libmimalloc-dev libxxhash-dev liburing-dev`.
  - Builds RelWithDebInfo.
  - Runs `ctest -LE slow -j1 --timeout 900 --output-junit` (:28-43).
  - Uploads the JUnit file and LastTest logs (:44-52).
- **Job `full`:**
  - Runs when `github.event_name != 'pull_request'` (main push, nightly, dispatch), 90 min limit.
  - Runs `ctest -j1 --timeout 2400` (:54-70).
- **What runs where:**

  | Event | fast tier (51 tests) | full suite (61 tests) |
  |---|---|---|
  | PR | yes | no |
  | push to main, nightly, dispatch | yes | yes |

  Counts are VERIFIED via ctest.
- **Not covered by CI:**
  - No sanitizer builds, no Debug build, no Seastar build (`SNC_SEASTAR` OFF), no lint, no coverage.
  - No R640 benchmarks or pilots (comment at ci.yml:5).
  - No required-status branch protection (CI.md:92-93, DOCUMENTED).
  - No retries, and no `continue-on-error` (CI.md:21-22).
- **Serial rationale:**
  - Port-block collisions and CPU timing interference (CI.md:24-42).
  - Running in parallel safely (port leasing) is named as future work (CI.md:41-42).
- **Contradictions in docs/CI.md (VERIFIED):**
  - The slow-tier list at CI.md:48-50 has 9 tests and omits `test_health_baseline`, which is labelled slow
    in `CMakeLists.txt:170`.
  - "PR #122: 40/40" (CI.md:80) and STATUS.md:329 "fast tier ... 40 tests ... full suite (49)" are stale.
    The suite is now 51 fast and 61 full.
  - CI.md:91 says "no TSAN run is on record", but the TSan logs in
    `bench/results/xnode-d2-review-fixes-devvm-20261110/suites/tsan-0[1-4].log` and
    `bench/results/s33-validation-devvm-20261108/tsan-run1-j1.log` say otherwise, as do
    STATUS.md:602-605 ("TSan 16/16", 2026-11-04). It may mean "in CI", but as written it contradicts the
    record.
  - CI.md:80-81 says "full tier has not yet run on a hosted runner". It since has
    (CI_FAILURE_DIAGNOSIS:9-10, run 43375591294, 47/49).

---

## 4. Benchmarks and experiment harnesses (short)

- **C++ tools** (`CMakeLists.txt:92-192`, VERIFIED):
  - `snc_bench`: in-process locality and durability;
  - `baseline_bench` and `baseline_server`: the conventional thread-pool and striped-map baseline;
  - `net_bench`: TCP client with `--rate` open-loop and `--mix`;
  - `snc_load`: the lab's transfer load plus `check`, the conservation checker (`bench/snc_load.cpp:1-5`);
  - `stage_stall`: reproduces the 100K false failover in one process;
  - `ckpt_walk_cost` (S2) and `ckpt_writeback_char` (S3-D);
  - `snc_bench_seastar`: only with `-DSNC_SEASTAR=ON`.
- **Drivers** (bench/, VERIFIED by `ls`):
  - matrix runners: `run_matrix.py`, `run_net_matrix.py`;
  - HA and replication runners: `run_failover.py`, `run_promotion.py`, `run_replication.sh`,
    `run_retries.py`, `run_seeding.py`, `run_standby_reads.py`;
  - `summarize.py`;
  - campaign and slice drivers: `pilot.py` (CP-1A), `s6.py` (S6), `s0_overhead.py`, `s1_move.py`,
    `p1_intake.py`;
  - `profile_check.py`: the profile guard;
  - `manifest.sh`: the host manifest;
  - `conventional/`: PostgreSQL SQL files, `run.py`, `run_replication.py`, `scylla/`.
- **Lab:** `bench/lab/lab.py`, which runs phases p0-p5 and soak over SSH, or on loopback through
  `loopback.toml` (`lab.py:1-24`). It comes with `trace_merge.py`, `cpu_sample.py`, `vultr_setup.sh` and
  three inventories.
- **Results** (`bench/results/`):
  - 48 directories and 32 loose CSV files (VERIFIED: `ls -d results/*/ | wc -l` = 48).
  - 33 result README.md files; 1 SHA256SUMS or MANIFEST file (find).
  - Kinds:
    - dev-VM slice validations (`s0`..`s41`, `intake-d1-*`, `xnode-d2-*`, `walcap-*`, `harness-races-*`);
    - R640 campaigns (`cp1a-*`, `s6-*`, `a2a-*`, `r640-*`);
    - one quarantined `INVALID-pilot-r640-20261106-stopped`;
    - lab runs (`lab-20261013`, `lab-20261104`);
    - characterizations (`failclosed-`, `health-`, `paxos-ballots-`, `ready-order-`, `health-baseline-20261115`, `k0-20261115`);
    - legacy matrix CSVs.
- **Manifests:** `bench/manifests/`, with one dev-VM and six R640 manifests plus a BIOS photo. The current
  one is manifest e, `r640-main-20261111.txt` (STATUS.md:124-127). VERIFIED.
- **Freeze and preregistration mechanism:**
  - `bench/pilot.py:265-305` refuses a real run unless all of these hold:
    - `runner_commit` is an ancestor of HEAD;
    - nothing outside `POST_FREEZE_ALLOWED = ("docs/", "AGENTS.md", "bench/cp1a_frozen.json", "bench/results/")`
      has changed since that commit;
    - the plan and manifest SHA-256 match;
    - `go_record` exists.

    VERIFIED.
  - It is tested by `tests/test_pilot.py:285-362`.
  - `bench/s6.py` uses `bench/s6_frozen.json` in the same way.
  - CP-1A is effectively dead from main. Per CI.md:97-104, the CI PR changed files outside the allowed
    set, so "a real CP-1A pilot from main refuses until a new review and freeze", and S6 already refuses.
    DOCUMENTED.
- **Staleness:**
  - `bench/cp1a_frozen.json` `_status` says "NOT FROZEN, NO GO" while `runner_commit` and `go_record` are
    set (lines 2-6, VERIFIED).
  - `bench/s6_frozen.json` `_status` says "NO GO" while `go_record` is set. VERIFIED.

---

## 5. Observability and operations

- **Gauges:**
  - `include/snc/gauges.h` (455 lines) defines a per-shard seqlock-published block, written only by its
    shard and readable without a shard task (`gauges.h:1-15`), plus checkpoint phase enums (`:27-40`).
  - It is observability only: "nothing here changes an admission, checkpoint, I/O or recovery decision"
    (`:14-15`). VERIFIED.
- **Admin verbs on `snc_server`** (one line in, one line out over TCP; `tools/admin_loop.h`):
  - Inline verbs, which never wait on shards:
    - `HEALTH` (`tools/snc_server.cpp:1092`; built at :939-960: role, term, uuid, epoch, leases,
      shards, worst shard stall);
    - `GAUGES [shard]` (:1099);
    - `DEBUG *` (debug-admin only).
  - Heavy verbs, run on a worker:
    - `STATUS` (:1216-1233: role, term, uuid, shards live/degraded/blocked, epoch, leases, unproven
      slots);
    - `RESOURCES` (:1235-1275: RSS/HWM, heap, waiters, overflow, emits, replication retained, WAL
      unacked/undurable, disk reader gauges, checkpoint running/count);
    - `AUDIT` (:1277; accounts total pending epoch held misrouted, :202-228);
    - `PROMOTE` (:1278).
  - The `MOVE ...` family goes through `tools/migrate.h`, including `MOVE ROLE` (:743).
  - Optional `KEEP 1`/`K <seq>` kept connections (K0).

  VERIFIED.
- **Witness verbs:** `DEGRADE`, `NOTREADY`, `READY`, `SYNCED`, `PLACEMENT`, `LEASES`, `DEBUG`, plus Paxos
  verbs (`tools/snc_witness.cpp:510-525`, grep of `verb ==`). VERIFIED.
- **`snc_ctl`:** `get`, `init`, `move`, `move --abort`, `placement backfill` (`tools/snc_ctl.cpp:1-31`).
  There is no health, status or dump command. VERIFIED.
- **Tracing:**
  - `SNC_TRACE=1` prints `T <mono ns> <pid> <event> k=v` lines to stderr. The flag is read once
    (TRACING.md:13-23; `include/snc/trace.h`).
  - There are 36 call sites.
  - `bench/lab/trace_merge.py` merges traces for a single host only.
  - TRACING.md status: "PROPOSED design, plus one as-built stopgap" (:3-5). Its limits: text through
    stderr, one host only.
  - The cost of disabled tracing is INFERRED, not measured (TRACING.md:24-26). HEALTH_BASELINE /
    STATUS.md:369-370 later say "under 1 ns per scope when off" was measured for ha::Caller. DOCUMENTED.
- **Logging:**
  - Free-text `std::fprintf(stderr, "snc: ...")`: 149 `fprintf(stderr` sites in src/ and tools/
    (VERIFIED grep).
  - The witness prints `t=%.2f group ...` to stdout (`snc_witness.cpp:511`).
  - No log levels, no structured or JSON logging, no syslog or journald (grep for
    prometheus, openmetrics, statsd, opentelemetry, syslog, journald, spdlog or json in src, tools and
    include: no hits). VERIFIED.
- **Structured errors:**
  - Wire status codes, `enum class Status` in `include/snc/types.h:25-47`: OK, E_NOT_FOUND, ... E_STALE,
    E_REPLAYED_OLD, E_NOT_OWNER, E_WRONG_NODE, E_MOVED, E_FENCED, E_MIGRATING.
  - Admin replies are text (`OK`, `NO <why>`).
  - Storage failures are fail-stop aborts (`storage.cpp:816-819`). VERIFIED.
- **Metrics export:** none beyond the admin `GAUGES`, `RESOURCES` and `STATUS` text lines. Nothing is
  pushed or scraped. VERIFIED by grep.
- **Configuration:**
  - Command-line flags only: about 161 flag mentions in `tools/snc_server.cpp` (grep count, rough).
  - Environment switches, from grep of `getenv("SNC_`: `SNC_TRACE`, `SNC_HA_KEEP`, `SNC_CKPT_IO`,
    `SNC_CKPT_SYNC_TOKENS`, `SNC_FOLLOWUP_INTAKE`, `SNC_XNODE_ACCOUNTING`, `SNC_CLUSTER_DEBUG`,
    `SNC_SEASTAR_MSG`, plus `SNC_MIGRATE_*` and `SNC_P` in tests.
  - The only config files are the lab's TOML inventories. VERIFIED.
- **Packaging and deployment:**
  - No `install()` in CMakeLists.txt.
  - No systemd unit, Dockerfile or compose file (find). VERIFIED.
  - Deployment is `lab.py deploy` (copying binaries over SSH) and `vultr_setup.sh`.
- **Backup and restore:** no tooling (grep of tools and src). Backup and PITR are explicitly not
  authorized (FUTURE_ARCHITECTURE.md:8-10). VERIFIED.
- **Security surface:**
  - D4 (audit): no authentication; admin, witness, seed and node-link ports listen on all interfaces
    (STATUS.md:322-323). DOCUMENTED.
- **What a failure diagnosis currently needs** (INFERRED from CI_FAILURE_DIAGNOSIS and TRACING):
  1. Reproduce the failure with `SNC_TRACE=1`.
  2. Collect every process's stderr.
  3. Merge the traces by monotonic clock (single host only).
  4. Poll `STATUS`, `GAUGES` and `RESOURCES` by hand or from the lab.
  5. Often add temporary diagnostic code: the CI diagnosis put gauge dumps and samplers on a throwaway
     branch, PR #124, "never to be merged" (CI_FAILURE_DIAGNOSIS:12,194-195).
  6. Read free-text log lines such as `standby shard 0 stopped acking`.

  There is no persistent event history, no flight recorder and no cross-host correlation, and
  `torn_tails` (silent WAL truncation) is not surfaced to operators.

---

## 6. Governing-document map

### 6.1 Contract and authority docs

| Doc | Kind | Authority (as the doc states) |
|---|---|---|
| `AGENTS.md` (3217 lines) | Architecture invariants (:18-67) plus a long handoff log (:153-) | Invariants are "accepted" (cited as `[INV n]` by CORE_CONTRACTS.md:17) |
| `docs/adr/SNC-ADR-RUST-001.md` | **The only ADR**, "Status: Accepted", 2026-11-15 (:3-4) | Sets the production direction (Rust, with C++ as the behavioral reference). "The specification is authoritative" (:26). **Authorizes no Rust work now** (:67-70) |
| `docs/CORE_CONTRACTS.md` | Contracts: identities, ownership, mutation stages, durability classes, read consistency, acks, retry, promotion, R08, R11, bounded work, recovery barriers, obligations, checkpoint (sections 1-14) | "**DRAFT for review**" (:3). Statements are tagged `[INV]`/`[ACCEPTED]`/`[AS-BUILT]`/`[PROPOSED]`, and PROPOSED "binds nothing until accepted" (:14-20) |
| `docs/FAILURE_MODEL.md` | Fault classes P1-P3, H1-H3, N1-N7, S1-S6, C1-C5+, with guarantees G1-G6 and evidence levels EL0-EL6 | G1-G6 "[PROPOSED as the v0.2 contract]" (:40-41); characterization |
| `docs/INTERACTION_MATRIX.md` | Pairwise fault x feature matrix, plus a ranked untested list (section M) | Characterization (EL labels) |
| `docs/EVIDENCE.md` | R01-R14 evidence map, retry contract, R08, contradictions, verification slice | Characterization; "changes no engine behavior" (:3-5) |
| `docs/STABILIZATION_REPORT.md` | Evidence-level definitions, sanitizer baseline, resource inventory | Characterization (the core stabilization work order) |
| `docs/EXPERIMENTAL_VALIDATION.md` | Falsification milestone, **scope freeze after S1** (section 3, :57) and GO/REFINE/NARROW/STOP (section 14) | Decision by cody, 2026-11-04; status still "PLANNED" (:3) |
| `docs/STATUS.md` | Running status ledger, scope freeze summary (:13-20), handoff §24 steps, spec deviations | De facto index of approvals; DOCUMENTED history |
| `docs/EXPERIMENTAL_BASELINE.md`, `BASELINE_READINESS.md`, `CAMPAIGN_PREP.md`, `PILOT_PLAN.md` (APPROVED), `S6_PREREG.md` | Profiles A-D, triage, pre-registered rules, readiness results | Approved plans and preregistrations; results |
| `docs/ARCHITECTURE_EVIDENCE_AUDIT_20261113.md` | Audit: defects D1-D4, risks R1-R…, milestones M1-M3 | Findings merged (PR #121); "Nothing is implemented" |
| `docs/ARCHITECTURE_AUDIT.md` (2026-10-13, Codex) | "Recommended design direction" (:8); KEEP witness HA, introduce placement | Historical direction, written by Codex |

### 6.2 Docs that say they authorize nothing (VERIFIED)

- `docs/FUTURE_ARCHITECTURE.md:3-10`: "VISION, NOT AUTHORIZED ... authorizes no implementation".
- `docs/STATE_NATIVE_COMPUTING_LINEAGE.md:3` ("Research / architecture exploration") and :647, :837
  ("hypotheses, not conclusions"). The phrase "authorizes no ... DSM implementation" is in STATUS.md
  (STATUS.md:504-508), not in the doc itself.
- `docs/handoff/AGENT_SNC_ARCHITECTURE_V0_1.md:3` ("design input for team review") and :1110
  ("design input, not uncontrolled implementation authorization").
- `docs/CONTROL_CONNECTIONS_DESIGN.md:3`: "design only; authorizes no code". K0 was later authorized
  separately (K0_TEST_PROBE.md:3-6).
- `docs/R640_TOPOLOGY.md`: "Nothing here is approved."
- `docs/research/*`: "builds nothing in Core by itself" (research/README.md:3-5). All seven deliverables
  are "not started" (:22-28).

### 6.3 Classification of the rest of docs/ (by status line, VERIFIED from each file's head)

- **Approved or accepted design, implemented:**
  - CONTROL_PLANE (accepted with clarifications);
  - HEALTH_DECOUPLING (ACCEPTED option B, IMPLEMENTED);
  - MIGRATION_FAIL_CLOSED (IMPLEMENTED, the D1/D7 slice);
  - MIGRATION_STAGING (ACCEPTED WITH MODIFICATIONS);
  - CROSS_NODE_EXCHANGE_DESIGN (rev4 APPROVED, implemented);
  - FOLLOWUP_INTAKE_DESIGN (rev2 approved with conditions);
  - ENGINE_LIFECYCLE (S3.2 approved);
  - S4_SYNC_TOKENS (COMPLETE);
  - SYNC_READINESS (IMPLEMENTED);
  - PAXOS_BALLOTS (F8 outcome A APPROVED);
  - K0_TEST_PROBE and HEALTH_BASELINE (approved by cody);
  - CHECKPOINT_BACKPRESSURE_DESIGN (header still "DESIGN-R, PROPOSED", but STATUS.md:34-35 says rev2
    approved; the header is stale).
- **Slice docs marked "for review" whose approval is recorded only in STATUS or handoff:**
  - S1_ADMISSION, S1_1_CORRECTIONS, S1_2_REPLAY, S2_PACING, S3_WRITEBACK;
  - S3D_WRITEBACK ("proposal for review"; STATUS says approved with conditions);
  - LEGACY_ROLE ("implemented, for review");
  - CPU_PLACEMENT_R1 ("review, plus one defect fix"; STATUS: PR #133 approved).

  The headers lag the approvals (INFERRED from STATUS.md:40-69 against the headers).
- **Characterization, results, root cause:**
  - BENCHMARKS, CONVENTIONAL, CROSSCORE, PREFETCH, SKEW, HUGEPAGES, SEASTAR, ROUTING, TRANSFERS,
    REPLICATION, RETRIES (built), CLUSTER, MIGRATION, REAL_MACHINES;
  - F1_Q10_LIVENESS_ROOT_CAUSE, CHECKPOINT_FAILOVER, TEST_MIGRATE_ROOT_CAUSE,
    TEST_MIGRATE_CONNECTION_CHURN, CI_FAILURE_DIAGNOSIS_20261114, CP1A_DRAFT_REVIEW.
- **Process:** CI.md (M1-0); TRACING.md (proposed design plus stopgap).
- **Working notes:** `docs/audit-20261113-working-notes/` (A-E notes, QA_REPORT, logs) and
  `docs/ci-m1-0-evidence/`.
- **docs/handoff/:**
  - About 100 files, mostly review and approval records (PR41 to PR135). These are the authority trail:
    each "approved by cody" lives in a handoff file.
  - Work orders: `SNC_Core_Stabilization_Complete_Work_Order.{docx,txt}`,
    `SNC_Experimental_Validation_Roadmap.txt` (source of EXPERIMENTAL_VALIDATION.md:5-7).
  - GO records: `CP1A_R640_GO_20261111.md`, `S6_R640_GO_20261111.md`.
  - `SNC_Architecture_Contracts_and_Validation_Handoff_v1.0.pdf` (2026-11-03). It says "All contracts
    below are proposals for review" (pdftotext :33). Its baseline reference is "Architecture and Research
    Vision v0.2" (reference [1], :21-23), which **is not in the repo** (grep: no hits).
  - `SNC_Future_Architecture_Vision_v2.{docx,txt}`: the source of FUTURE_ARCHITECTURE.md.
  - Latest approvals: `PR134_K0_Review_Approval.md`, `PR135_D2a_Default_Shards_Review_Approval.md`.

### 6.4 Staleness and contradictions checked

| Item | Evidence | Label |
|---|---|---|
| README layout table omits `src/seastar/`, `docs/`, `.github/`, `tests/fixtures`; says `tools/` = "`snc_server`" though `snc_ctl`, `snc_witness`, `admin_loop.h`, `ha.h`, `migrate.h`, `paxos.h`, `control.h` are there; `src/core/` "Shard execution, dispatch, allocator, registry" omits storage, replication, cluster, placement, node_link, seed, session | README.md:72-83 vs `ls tools include/snc`, CMakeLists.txt:18-39 | VERIFIED |
| README and `wire.h` say routing is "never a correctness requirement"; audit D1 says it is contradicted for unsequenced pipelined clients | README.md:57-58; include/snc/wire.h:4-5; ARCHITECTURE_EVIDENCE_AUDIT:48 | VERIFIED (doc conflict) |
| STATUS "All 16 ctest tests pass (2026-11-04)" and sanitizers 16/16; now 61 tests | STATUS.md:601-605; `ctest -N` | VERIFIED stale |
| STATUS and CI fast/full counts 40/49 | STATUS.md:329; CI.md:80 | VERIFIED stale |
| CI.md slow list missing test_health_baseline | CI.md:48-50 vs CMakeLists.txt:170 | VERIFIED |
| CI.md "no TSAN run is on record" vs recorded TSan suites | CI.md:91 vs bench/results/xnode-d2-review-fixes-devvm-20261110/README.md:16 | VERIFIED |
| `cp1a_frozen.json`/`s6_frozen.json` `_status` text vs set fields | files lines 2-6 | VERIFIED |
| EXPERIMENTAL_VALIDATION "Status: PLANNED", though its campaigns (CP-1A, S6, A2-a) ran | EXPERIMENTAL_VALIDATION.md:3; STATUS.md:168-193 | VERIFIED stale header |
| CHECKPOINT_BACKPRESSURE_DESIGN header "PROPOSED" vs STATUS "revision 2 approved (PR #68)" | header :3; STATUS.md:34-35 | VERIFIED |
| CORE_CONTRACTS is still a "DRAFT for review"; the ADR makes "the specification" authoritative, but the in-repo contracts are a draft and the spec is outside the repo | CORE_CONTRACTS.md:3; ADR :26 | VERIFIED |

### 6.5 Identifier collisions (each meaning with its source; VERIFIED by reading the line)

- **D1**
  1. Audit defect: cross-shard overtaking (ARCHITECTURE_EVIDENCE_AUDIT_20261113.md:48).
  2. A moved slot served from a partial or stale copy, the "D1/D7" pair (MIGRATION_FAIL_CLOSED.md:1,38; FAILURE_MODEL.md:103 also D1b-D1d).
  3. Catch-up reader placement defect (CPU_PLACEMENT_R1.md:22,54).
  - Also confusable with **D.1**, the follow-up intake slice (FOLLOWUP_INTAKE_DESIGN; STATUS.md:82-87).
  - CI_FAILURE_DIAGNOSIS:193 "D1 to D4 are not started" means (1).
- **D2**
  1. Audit: silent loss on last-segment WAL corruption (AUDIT:49).
  2. Default shard count from the host CPU count, slice D2-a (CPU_PLACEMENT_R1.md:39,56).
  3. The lab window metric undercount (MIGRATION_STAGING.md:933).
  - Also confusable with **D.2**, cross-node exchange.
- **D3**
  1. Audit: `client_id` reuse false "OK, replayed" (STATUS.md:321-322).
  2. The cluster/control default shard count should come from the map (STATUS.md:382; CPU_PLACEMENT_R1).
  3. MIGRATION_STAGING D3.
- **D4**
  1. Audit: no authentication, all interfaces (STATUS.md:322-323).
  2. An Engine-constructor refusal aborts snc_server (STATUS.md:382-383).
  3. MIGRATION_STAGING D4 (single flight).
- **D5**
  1. A shard-count refusal is quiet (STATUS.md:383).
  2. MIGRATION_STAGING D5 (slot size limit; AGENTS.md).
- **F1**
  1. Server fix for q10 liveness (CI_FAILURE_DIAGNOSIS:107; F1_Q10_LIVENESS_ROOT_CAUSE.md).
  2. Replication retention after a grant (BASELINE_READINESS.md:35,311; STATUS.md:432).
  3. The open-loop `--rate` fix (CP1A_DRAFT_REVIEW.md:5,54).
  4. A test_fail_closed scenario F1 (FAILURE_MODEL.md:103 "F1-F11, F13 ...").
  5. D.2 rev1 review finding "END on a foreign purge" (CROSS_NODE_EXCHANGE_DESIGN.md:1540-1545).
  6. A FOLLOWUP_INTAKE measurement F1 (:464).
- **F2-F4**
  - CI diagnosis: checker, X16 test, X3/X26 diagnosis (CI_FAILURE_DIAGNOSIS:113,143,182).
  - Also fail-closed scenarios and CROSS_NODE F2 (two incarnations) and F3 (silence vs pausing)
    (CROSS_NODE_EXCHANGE_DESIGN; AGENTS.md:2159-2160).
  - Also STATUS.md:434 "F2: done. The R11 contract ...".
- **F8**
  1. Paxos ballots after a witness restart (PAXOS_BALLOTS.md:1).
  2. A test_intake/D.1 test "F8 tests D.1" (FOLLOWUP_INTAKE_DESIGN.md:421).
  3. A fail-closed scenario.
- **F9 and F10**
  - F9: PAXOS configuration exclusion (PAXOS_BALLOTS.md:3) vs FOLLOWUP_INTAKE F9 (:444).
  - F10: contract death tests (tests/test_intake.cpp:4) vs PAXOS F10 legacy lines vs fail-closed F10.
- **S1**
  1. Bounded destination staging (MIGRATION_STAGING; `tests/test_s1.py` header; PR #44).
  2. Resource-specific admission (S1_ADMISSION.md; `tests/test_s1_admission.cpp` header; PR #72).
  3. FAILURE_MODEL storage fault S1, "process death before durable" (FAILURE_MODEL.md:81-90).
- **S6**
  1. The R640 checkpoint regression campaign (S6_PREREG.md).
  2. FAILURE_MODEL storage fault S6, latent corruption (FAILURE_MODEL.md:81-90).
- **P1**
  1. Process-crash fault class (FAILURE_MODEL.md:57).
  2. Lab phase "one node" (REAL_MACHINES.md:236).
  3. Multi-process CPU stacking (CPU_PLACEMENT_R1.md:3,28).
  4. S2 pacing tests P1-P8 (test_s2_pacing.cpp:5).
  5. The D.1 performance matrix P1 (STATUS.md:85).
- **X1**
  1. INDEX wire kind (EXPERIMENTAL_BASELINE.md:24,78; test_index_wire).
  2. D.2 test "stalled remote" (CROSS_NODE_EXCHANGE_DESIGN.md:1372).
- **G2**
  1. Guarantee "HA-durable ack survives" (FAILURE_MODEL.md:46).
  2. The witness campaign guard (PAXOS_BALLOTS.md:180,318; test_lab_witness_guard).
- **C1-C5**
  - FAILURE_MODEL control-plane faults.
  - Recovery contract C4/C12/C13 (STATUS.md:567,588-589).
  - PILOT_PLAN corrections C1-C3.
  - CHECKPOINT_BACKPRESSURE option C1 `sync_file_range`.
  - K0 client tests C1-C7.
- **A/B/C letters**
  - Profiles A-D (STATUS.md:399).
  - Validation workloads A/B/C (EXPERIMENTAL_VALIDATION.md:197; research/README.md:13-15).
  - Research lanes A-E (research/README.md:22-26).
  - A2/A2-a: control-connections acceptance A2 vs the A2-a reproducibility campaign.
- **R-labels**
  - EVIDENCE R01-R14 (spec risks, e.g. R08 async, R11 session).
  - Audit risks R1, R2 (AUDIT:351-352).
  - CPU_PLACEMENT R1 and R2 (workstreams).
  - S1 R1 checks.
- **H1**
  - FAILURE_MODEL host crash.
  - BASELINE_READINESS/PAXOS H1-H14.
  - HEALTH_BASELINE H1 and H2.
- **M1**
  - Audit milestone M1 (AUDIT:146), with M1-0 = CI.
  - MIGRATION_FAIL_CLOSED move id M1.
- **K1** means different things in CONTROL_CONNECTIONS_DESIGN and STABILIZATION_REPORT.

---

## 7. The external specification

- README.md:8-9 and STATUS.md:3-5 point to "SNC v0.1 Object & Execution Specification" at
  `https://claude.ai/code/artifact/8a25e96e-...`. These are the only two occurrences
  (`grep -rn "8a25e96e"`). VERIFIED.
- No copy is in the repo:
  - `grep -rn "Object & Execution\|Object and Execution"` finds only those two links;
  - `find -iname "*spec*"` finds nothing;
  - `grep -rln "SNC_Project_Handoff\|Project_Handoff_v0.1"` finds nothing;
  - `git log --all -- '*spec*' '*Project_Handoff*'` returns nothing.

  VERIFIED.
- The code and docs cite it constantly ("spec section 12" in `include/snc/wal_format.h:1`, "spec
  section 5" in STATUS.md:576, "handoff §24" in STATUS.md:5,515, "the v0.1 storage spec's rule C7" in
  FAILURE_MODEL S3). 43 hits for "spec section / handoff § / the handoff's" (grep count).
- The "Architecture and Research Vision v0.2" that the v1.0 contracts PDF names as its baseline
  reference [1] is also absent (VERIFIED by grep).
- **Finding: the authoritative specification is not in the repository.** The accepted ADR makes "the
  specification" authoritative for C++/Rust discrepancies (ADR :26). The in-repo stand-in,
  CORE_CONTRACTS.md, is a DRAFT.

---

## 8. Gaps for a Rust differential-testing program (factual list, no design)

1. **No in-repo, language-neutral specification.** The spec is external (section 7), and
   CORE_CONTRACTS.md and FAILURE_MODEL G1-G6 are DRAFT or PROPOSED.
2. **No executable reference model or oracle** for op semantics. The only checkers are money
   conservation, single ownership and `AUDIT` (EVIDENCE.md:33).
3. **No operation-history recording or history checker** (linearizability or serializability). There is
   no recorded history format to compare two implementations on.
4. **No deterministic simulation or controlled scheduling.** Runs are not replayable: `--seed` affects
   only soak (EVIDENCE.md:33).
5. **Most fault injection is C++-internal and not reusable against another implementation.** About 30
   `Engine::debug_*` static hooks (`include/snc/snc.h:668-715`), `LinksGeneration::debug_crash_after`
   and the in-process LinkProxy all drive the C++ Engine API directly: the 28 C++ tests, 22 of them IP.
   Only the 25 Python suites, the `DEBUG` admin verbs and the lab work at the process and wire boundary.
6. **Python suites are coupled to C++ specifics:**
   - binary names in the build dir;
   - free-text stderr lines (e.g. test_ckpt_failover.py:50);
   - CMakeCache sanitizer detection (test_health.py:308-313);
   - `net_bench` and `snc_load` output formats.
7. **No golden corpus of on-disk artefacts** (superblock, WAL segments, checkpoint manifest or parts)
   or wire frames for cross-implementation compatibility. Formats exist only as C++ headers
   (`include/snc/wal_format.h`, `include/snc/wire.h`). INFERRED: no fixture dir other than conntrack.
8. **No fuzzing** of the wire protocol, admin line protocol, WAL recovery or placement text form.
9. **No WAL I/O-error, power-loss or media-corruption injection.** The durability baseline is
   SIGKILL-only (STATUS.md:596; FAILURE_MODEL H3, S3, S5, S6).
10. **No sanitizer CI.** TSan and ASan runs are manual and per slice (CI.md:88-91).
11. **No conformance labels.** Ctest labels are only `slow`, with no tag for "contract test" versus
    "C++-internal test". Timing-bound tests (test_health_baseline, test_ckpt_failover,
    test_net_bench_rate) encode C++ performance envelopes, not contracts. HEALTH_BASELINE separates
    "measured behavior" from "intended guarantee" (STATUS.md:366-369).
12. **Flaky tests would make differential comparison noisy:** test_xnode X16/X3/X26 are unresolved
    (F3, F4 not started), and some hosted runs are CPU-sensitive (section 1.3).
13. **No machine-readable observability.** There is no metrics export, no structured logs and no
    cross-host trace correlation to compare internal state between implementations (section 5).
14. **The decisive spec artefacts are missing from the repo:** the external spec, the v0.1 handoff
    (§24) and the Vision v0.2 (section 7).
