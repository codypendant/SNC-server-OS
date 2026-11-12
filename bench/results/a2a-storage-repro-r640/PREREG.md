# A2-a storage-state reproducibility validation: preregistration (for review, NOT RUN)

Authorized to **prepare, not run**, by
[docs/handoff/PR113_Storage_State_Final_Review.md](../../../docs/handoff/PR113_Storage_State_Final_Review.md)
("NEXT AUTHORIZED REPOSITORY TASK"). It returns for independent review **before the first explicit trim or
performance session**. The method context is
[METHOD_PROPOSAL.md](../r640-storage-state-20261112/METHOD_PROPOSAL.md), revision 2.

- **Method validation (B1).** The result shows whether the A2-a start state controls session-to-session
  movement. It also gives a candidate N_raw.
- **Not the campaign objective.** 654,569 ops/s is a historical anchor, and the session medians are not the
  campaign p99 objective.
- **SNC only.** No competitor, no threshold adjustment after data, no product change. `bench/pilot.py`,
  `bench/pilot_plan.json` and the binaries are unchanged.

## A0 (already done; precondition)

`fstrim.timer` was disabled and made inactive on 2026-11-12 at 03:54 UTC
([a0-fstrim-timer/a0.txt](a0-fstrim-timer/a0.txt)). No trim has run since 2026-11-11 15:09. No other trim
path exists ([a0-fstrim-timer/other-trim-paths.txt](a0-fstrim-timer/other-trim-paths.txt)): no cron job,
`/etc/fstab` `defaults` with no `discard`, and `e2scrub_all` only scrubs LVM-backed ext4. The driver refuses
unless the timer is still `disabled`/`inactive`.

## Design

**K = 5 physical sessions**, back to back, in one driver invocation. Each session k = 1..5:

1. **Clear:** remove `/srv/snc-data/a2a`, so nothing of ours is on the filesystem.
2. **A1 pre-trim snapshot** (read only; listed below).
3. **Explicit logged trim:** `sudo -n /usr/sbin/fstrim -v /srv/snc-data`. Its output and time are recorded.
   A non-zero exit aborts the validation (INCOMPLETE).
4. **Idle settle: 30 min** (1,800 s). The driver runs nothing. The data disk's diskstats are recorded before
   and after; written and discarded sectors are reported, not judged.
5. **A1 start snapshot.**
6. **12 Profile-A runs**, the same fixed order in every session:

   | slot | S0 | F1 | C1 | F2 | C2 | F3 | C3 | F4 | C4 | F5 | C5 | S1 |
   |---|---|---|---|---|---|---|---|---|---|---|---|---|
   | kind | sentinel | fixed | capacity | fixed | capacity | fixed | capacity | fixed | capacity | fixed | capacity | sentinel |

   - **Fixed (F) and sentinel (S):** CP-1A's fixed-rep run. Open loop at **654,569 ops/s**, `--depth 1024`,
     10 s warm-up, 60 s window, 10 s drain, with the memory measurement (`Pilot.one(..., memory=True)`).
   - **Capacity (C):** CP-1A's capacity run. Closed loop, `--depth 512`, 60 s, throughput only.
   - Fixed and capacity reps alternate, so neither kind sits at one end of a session. The sentinels sit at
     both ends.
7. **A1 end snapshot.** It includes the host units that systemd started since the session began (report
   only).

Each run gets a fresh server and data dir. All other settings are CP-1A Profile A, unchanged, from
`bench/pilot_plan.json` through `bench/pilot.py` (runner commit `4250b32`): 5 M objects; mix 70/15/10/5;
owner routing; 8 shards; 8 client threads; PILOT-L; manifest e; the Profile-A checkpoint (W = 1, pacing,
dsync); and the profile guard.

`test_aduration`'s approach is reused. `test_a2a.py` A2 checks that every server and client command equals
CP-1A `A-fixed-r0` or `A-capacity-r0` with ports, data dir and ids masked.

**Paths:**
- data `/srv/snc-data/a2a` (on `sdb`, the trimmed filesystem);
- scratch `/dev/shm/snc-a2a`;
- out `/home/solo/a2a-storage-repro-r640` (outside the tree; copied in afterwards, unchanged).

**Expected time:** about 50 min per session (trim about 1 min, settle 30 min, 12 runs about 18 min), so
about **4.2 h** in all.

### A1 snapshot (read only; review "A1 session snapshots")

Each snapshot records:
- `/proc/diskstats` (sectors written and discarded on `sdb`);
- `smartctl -x /dev/sdb`, parsed for health, temperature, background-scan status, progress and count,
  endurance used, grown defects, non-medium errors, write GB processed, and uncorrected errors;
- `sg_logs -a /dev/sdb`;
- the last 20 `fstrim.service` journal lines and `fstrim.timer` enablement;
- `tune2fs -l /dev/sdb1` lifetime writes;
- `/proc/uptime` and the boot id.

No counter is cleared. A background scan that is present, or whose progress advances during a session, is
recorded explicitly. It is not a verdict input by itself; its effect shows through the CV and sentinel
rules.

### Root access

Root is used only through `sudo -n`, for exactly the 4 commands of `a2a.py` `ROOT`, as granted by the
proposed `sudoers-snc-a2a`: `fstrim -v /srv/snc-data`, `smartctl -x /dev/sdb`, `sg_logs -a /dev/sdb` and
`tune2fs -l /dev/sdb1`.
- **Not installed yet.** It is installed with `visudo -c` only after this preregistration is approved.
- The driver refuses if `sudo -n -l` cannot run any of them.

### Preflight

1. `pilot.Pilot` runs the full CP-1A real-mode preflight unchanged. That covers the freeze gate, clean
   tree, rebuild, RelWithDebInfo, governors `performance`, `numa_balancing` 0, manifest e, isolation and
   the committed plan.
2. The driver adds these refusals:
   - binaries, plan and manifest must equal the CP-1A record;
   - `fstrim.timer` must be disabled and inactive;
   - the 4 sudo rules must be present;
   - `/srv/snc-data/a2a` must be on the `/srv/snc-data` mount, whose data disk is `[sdb]`.

## Decision (fixed before data; review B1–B4)

Per session k, from its own reps:
- S_p99,k = median total p99 of F1..F5; CV_p99,k = `pilot.cv`(F1..F5 total p99);
- S_thr,k = median total ops/s of C1..C5; CV_thr,k = `pilot.cv`(C1..C5 total ops/s);
- the sentinels P_start = S0 total p99 and P_end = S1 total p99.

Across sessions:
- CV_p99_between = `pilot.cv`(S_p99,1..5); CV_thr_between = `pilot.cv`(S_thr,1..5).
- **N_raw = 3 × max(CV_p99_between, CV_thr_between, max_k CV_p99,k, max_k CV_thr,k)** (B2: the maximum,
  not the median).

**STABLE**, meaning A2-a reproducible, holds only if all of these do:
1. All 60 runs are present (no abort).
2. Every fixed and sentinel rep is sustained by `pilot.sustained()` (0 errors, 0 refusals, ≥ 99% completed,
   end backlog ≤ 1%, drained). Every capacity rep has no failure and 0 errors.
3. **B3:** CV_p99_between ≤ 10%, CV_thr_between ≤ 10%, and every CV_p99,k ≤ 10% and CV_thr,k ≤ 10%. A CV
   strictly above 10% stops.
4. **B3, multimodality operationalized:** for p99 and for throughput separately, sort the 5 session
   medians. No adjacent gap may exceed **2 × the widest within-session range** (max − min of one session's
   5 reps) of that metric. A larger gap means the sessions fall into separated groups.
5. **B4, symmetric sentinels:** for every session, max(P_end / P_start, P_start / P_end) ≤ 1 + N_raw,
   compared exactly. Equality passes, and a large improvement stops too.

**STOP** if any of 2–5 fails: return for methodology review. N is not widened to absorb it, and no session
is rerun or replaced. **INCOMPLETE** (an abort, including a failed trim): STOP and return for review.

**Reported, not judged:**
- `anchor_median_of_session_medians_p99_ns`, the historical anchor only (B1). It becomes the campaign
  objective only after the stable-peak revalidation, and by a later preregistered step.
- Per-rep p99.9, max and per-kind latencies; CPU and write bytes per op; memory per object.
- Settle-window diskstats deltas; background-scan progress per snapshot; SSD temperature; host units
  started during each session.

`a2a.py judge OUT` recomputes the verdict from `pilot.json`.

## Design points for the reviewer

1. **The sentinel bound uses N_raw from this same validation.** B4 needs an N, and no accepted N exists
   before this run. The CP-1A-only D = 0.057858 was ruled out (PR #113 review, C). The alternative is a
   fixed value set now, for example 10%.
2. **Multimodality rule (B3, last bullet):** the "visibly multimodal" condition becomes deterministic as a
   gap of more than 2 × the widest within-session range. The reviewer may still rule on visible
   multimodality from the per-rep tables, in addition.
3. **An unsustained anchor rep stops the validation.** 654,569 sustained 8 of 8 times in CP-1A and 9 of 9
   in the duration session. If it no longer sustains under A2-a, the stable-peak premise is in question
   before any CV means anything.
4. **One preflight at the start, not per session.** Governors, NUMA and the clean tree are checked once.
   The A1 snapshots record the per-session state.
5. **Host timers stay enabled.** `man-db` ran at 02:01:40, inside the duration session; `apt-daily` and
   `motd-news` ran at 23:31–23:33, during CP-1A's Profile C. They touch the boot disk, not `sdb`. They are
   recorded per session but not masked. Masking them would be a further host change.
6. **The sudoers install** (above) is needed before the run.
7. **Unrelated to this package:** `test_pilot` has been red on `main` since #110 (`tests/test_pilot.py:366`).

## Validation before data (this commit)

- `test_a2a.py` passes. It starts no process and trims nothing.
  - **A1:** constants; the order; that `sudoers-snc-a2a` grants exactly `ROOT`; and the SMART parser
    against the real 2026-11-12 capture.
  - **A2:** the session procedure in exact sequence, the commands against CP-1A, the memory path, failed
    trim → INCOMPLETE with nothing after it, and the preflight refusals.
  - **A3:** the verdict boundaries listed above.
- **Mutations:** `negative/summary.txt`, 19 mutants of `a2a.py`. **19 / 19 caught.**
  - The first pass caught 17. `fixed-memory-false` and `within-median` survived. The test then gained a
    memory-path check and a pinned max-within case, and the suite was rerun.
  - The test also found one driver bug before data: the verdict's per-session table overwrote the
    session records. It was fixed by renaming the key to `session_stats`.
- No file outside `bench/results/a2a-storage-repro-r640/` changes in this commit, except
  `METHOD_PROPOSAL.md` (revision 2), `docs/STATUS.md` and `AGENTS.md`. All are in the CP-1A post-freeze
  allowlist.

## Commands (after approval only)

```
sudo install -m 0440 bench/results/a2a-storage-repro-r640/sudoers-snc-a2a /etc/sudoers.d/snc-a2a && sudo visudo -c
cd /home/solo/SNC-server-OS        # HEAD = the approved preregistration merge, tree clean
PYTHONDONTWRITEBYTECODE=1 python3 -B bench/results/a2a-storage-repro-r640/test_a2a.py
python3 -B bench/results/a2a-storage-repro-r640/a2a.py run /home/solo/a2a-storage-repro-r640 \
    --scratch /dev/shm/snc-a2a --manifest bench/manifests/r640-main-20261111.txt --build build-s6
```
