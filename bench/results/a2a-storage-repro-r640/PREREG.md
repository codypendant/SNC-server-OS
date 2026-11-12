# A2-a storage-state reproducibility validation: preregistration, revision 3 (for review, NOT RUN)

Authorized to **prepare, not run**, by
[docs/handoff/PR113_Storage_State_Final_Review.md](../../../docs/handoff/PR113_Storage_State_Final_Review.md).
**Revision 2** applies every correction of
[docs/handoff/PR114_A2a_Prereg_Review1.md](../../../docs/handoff/PR114_A2a_Prereg_Review1.md)
(CHANGES REQUIRED; items 1–10). It returns for review **before the first explicit trim or performance
session**. Method context: [METHOD_PROPOSAL.md](../r640-storage-state-20261112/METHOD_PROPOSAL.md),
revision 2.

- **Method validation (PR #113 review, B1).** The result shows whether the A2-a start state controls
  session-to-session movement. N_raw is a result of the experiment.
- **Not the campaign objective.** 654,569 ops/s is a historical anchor.
- **SNC only.** No competitor, no CRITERIA, no REFINE #2, no threshold change after data, no product
  change. `bench/pilot.py`, `bench/pilot_plan.json` and the binaries are unchanged.

## Revision 3 (PR #114 review 2: one blocking correction)

[docs/handoff/PR114_A2a_Prereg_Review2.md](../../../docs/handoff/PR114_A2a_Prereg_Review2.md) accepted
everything in revision 2 except one point, which revision 3 fixes. It also adopts the review's optional
recommendation.

| Change | Code / test |
|---|---|
| **Both `sync -f /srv/snc-data` calls fail closed** through `sync_fs(rec, stage)`. It records rc, output and time in the session record (`sync_pre_trim`, `sync_post_trim`), and a nonzero rc raises `Incomplete`. A **pre-trim** sync failure stops before the A1 pre-trim snapshot and before fstrim. A **post-trim** sync failure stops before the 30 min settle. | `sync_fs`; A3 cases `sync="pre"` and `sync="post"`; mutants `sync-*` (6) |
| **Optional, adopted:** the per-session guard also requires `systemctl is-active fstrim.service` = `inactive`. This closes the route of a manually started fstrim already running when a session begins. It does not replace the trim proof. | `observe`, `guard_problems`; A2 drift `service`; A3; mutant `guard-fstrim-service-ignored` |

Mutation suite: **54 / 54 caught on the first pass** (revision 2's 47 plus 7).

## What changed in revision 2 (review 1 item → where)

| Item | Change | Code / test |
|---|---|---|
| 1 Sentinel | **Fixed `SENTINEL_LIMIT = 11/10`**: max(P_end/P_start, P_start/P_end) ≤ 1.10, exactly, equality passes. Independent of N_raw. | `judge`; A4; mutants `sentinel-*` |
| 2 Per-session guard | Before every session's trim, a fail-closed identity/state guard. No rebuild. | `observe`, `guard_problems`; A2, A3; `guard-*` |
| 3 Trim proof | rc 0, `/srv/snc-data` named in the output, reported bytes > 0, `sdb` discard sectors increased. | `trim_problems`; A3; `trim-*` |
| 4 Settle | **Judged:** `sdb` sectors written +0 and discarded +0 over the 1,800 s, else INCOMPLETE. | `settle_problems`; A3; `settle-*`; `pre-data/idle-baseline.txt` |
| 5 Capacity validity | returncode 0, not failed, errors 0, stale 0, index_errors 0. EXEC overload is expected. | `capacity_problems`, `run_problems`; A3, A4 |
| 6 A1 fail closed | Each snapshot is validated field by field. smartctl is judged on its parsed fields, not its bitmask rc. | `a1_problems`; A1, A3; `a1-*` |
| 7 Multimodality | The gap rule stays, as a **supplementary** automatic STOP signal only (see Decision). | text |
| 8 Order | Unchanged; the same order in every session, deliberately. | — |
| 9 A0 | Accepted. The timer is never re-enabled during the campaign; the guard enforces it. | — |
| 10 Sudoers | Unchanged design. Installed only after final approval; the `visudo -c` result is recorded at install. | — |

## A0 (done; accepted)

On 2026-11-12 at 03:54 UTC, `fstrim.timer` was disabled and made inactive
([a0-fstrim-timer/a0.txt](a0-fstrim-timer/a0.txt)). There is no other trim path
([a0-fstrim-timer/other-trim-paths.txt](a0-fstrim-timer/other-trim-paths.txt)).

Idle baseline ([pre-data/idle-baseline.txt](pre-data/idle-baseline.txt), read only):
- Over about 63 min idle (03:20–04:24 UTC), `sdb` wrote **0 sectors** and discarded **0 sectors**. The only
  I/O was one zero-sector cache flush.
- No `ext4lazyinit` thread exists.

So a clean settle is achievable on this mount without a tolerance.

## Design

**K = 5 physical sessions**, back to back, in one driver invocation. One full CP-1A real-mode preflight
(`pilot.Pilot`) runs at the start. It includes the freeze gate and the only rebuild. Its observed boot id and
HEAD become the guard reference.

Each session k = 1..5, **fail closed**. Any integrity failure aborts the whole validation as INCOMPLETE,
and no session is replaced.

0. **Guard (item 2).** Read directly, with no rebuild. Every check must hold:
   - same boot id and git HEAD as the start;
   - `git status --porcelain` empty;
   - `fstrim.timer` `disabled` and `inactive`, and `fstrim.service` `inactive` (revision 3);
   - `kernel.numa_balancing` = 0;
   - every CPU governor `performance`;
   - `findmnt /srv/snc-data` = `/dev/sdb1 /srv/snc-data ext4 rw,relatime,stripe=2`, and the data root
     resolves to that mount;
   - the physical data disk is `[sdb]`;
   - SHA-256 of `bench/pilot_plan.json`, manifest e, `build-s6/snc_server` and `build-s6/net_bench` equal
     the CP-1A record.

   The result is recorded per session.
1. **Clear:** remove `/srv/snc-data/a2a`, then `sync -f /srv/snc-data`. **rc 0 required**; otherwise
   INCOMPLETE before the A1 pre-trim snapshot and the trim (revision 3).
2. **A1 pre-trim snapshot**, validated (item 6).
3. **Explicit trim, proven (item 3):**
   - read the `sdb` discard sectors;
   - run `sudo -n /usr/sbin/fstrim -v /srv/snc-data`;
   - read the discard sectors again;
   - require rc 0, `/srv/snc-data: … (N bytes) trimmed` with N > 0, and a discard-sector delta > 0.

   The rc, stdout, parsed bytes and both counters are recorded. On failure the session does not settle.
   Then `sync -f /srv/snc-data` again. **rc 0 required**; otherwise INCOMPLETE before the settle
   (revision 3).
4. **Settle, judged (item 4):** read the `sdb` counters, sleep 1,800 s, read them again. Written-sector
   delta = 0 and discarded-sector delta = 0 are both required. Boot-disk activity is not judged; the host
   units started are recorded in A1.
5. **A1 start snapshot**, validated.
6. **12 Profile-A runs**, the same fixed order in every session (item 8):

   | slot | S0 | F1 | C1 | F2 | C2 | F3 | C3 | F4 | C4 | F5 | C5 | S1 |
   |---|---|---|---|---|---|---|---|---|---|---|---|---|
   | kind | sentinel | fixed | capacity | fixed | capacity | fixed | capacity | fixed | capacity | fixed | capacity | sentinel |

   - **F and S:** CP-1A's fixed-rep run. Open loop at **654,569 ops/s**, `--depth 1024`, 10 s warm-up, 60 s
     window, 10 s drain, with the memory measurement (`Pilot.one(..., memory=True)`).
   - **C:** CP-1A's capacity run. Closed loop, `--depth 512`, 60 s.
   - **Any run with a nonzero client exit or an unparsable report stops the validation at once**
     (INCOMPLETE). pilot's own aborts, a server exit or a failed load, do the same.
7. **A1 end snapshot**, validated.

All other settings are CP-1A Profile A, unchanged: 5 M objects; mix 70/15/10/5; owner routing; 8 shards;
8 client threads; PILOT-L; manifest e; the Profile-A checkpoint (W = 1, pacing, dsync); and the profile
guard. `test_a2a.py` A3 checks every server and client command against CP-1A `A-fixed-r0` /
`A-capacity-r0`, with ports, data dir and ids masked.

**Paths:**
- data `/srv/snc-data/a2a`;
- scratch `/dev/shm/snc-a2a`;
- out `/home/solo/a2a-storage-repro-r640` (outside the tree; copied in afterwards, unchanged).

**Expected time:** about 50 min per session, so about 4.2 h in all.

### A1 snapshot: content and validity (item 6)

Recorded:
- `/proc/diskstats`;
- boot id and `/proc/uptime`;
- `smartctl -x /dev/sdb`;
- `sg_logs -a /dev/sdb`;
- `tune2fs -l /dev/sdb1`;
- the last 20 `fstrim.service` journal lines;
- the host units systemd started since the session began (start and end snapshots, report only).

No counter is cleared. A snapshot is **usable** only if:
- diskstats parse for `sdb`;
- the boot id is a UUID;
- uptime parses;
- the SMART fields health, temperature, background-scan status, endurance used, grown defects, non-medium
  errors, and uncorrected read and write errors are all present, plus scan progress when the status is
  `scan is active`;
- `sg_logs` exits 0 and contains its Background scan results page;
- `tune2fs` exits 0 and reports Lifetime writes.

**smartctl's exit status is not required to be 0.** It is a bitmask, and it was 4 with full data on this
device on 2026-11-12. Its parsed fields are validated instead. An unusable snapshot is INCOMPLETE.

### Root access (item 10)

Root is used only through `sudo -n` for exactly the 4 commands of `a2a.py` `ROOT`, as granted by
`sudoers-snc-a2a` (syntax-checked with `visudo -c -f`: parsed OK):
- `fstrim -v /srv/snc-data`;
- `smartctl -x /dev/sdb`;
- `sg_logs -a /dev/sdb`;
- `tune2fs -l /dev/sdb1`.

There is no generic shell and no wildcard. It is **not installed** until final approval. At install, the
`visudo -c` result is recorded in the evidence. The driver refuses if `sudo -n -l` cannot run any of the 4.

## Decision (fixed before data)

Per session k:
- S_p99,k = median total p99 of F1..F5; CV_p99,k = `pilot.cv`(F1..F5);
- S_thr,k = median total ops/s of C1..C5; CV_thr,k = `pilot.cv`(C1..C5);
- the sentinels P_start = p99(S0) and P_end = p99(S1).

Across sessions:
- CV_p99_between = `pilot.cv`(S_p99,1..5); CV_thr_between likewise;
- **N_raw = 3 × max(CV_p99_between, CV_thr_between, max_k CV_p99,k, max_k CV_thr,k)**. N_raw is a
  **result only**. It judges nothing in this experiment and may inform CRITERIA later.

**INCOMPLETE** means an execution-integrity failure. It stops immediately, with no replacement session.
Causes:
- a guard drift;
- a failed `sync -f` (pre- or post-trim);
- a trim not proven;
- a settle not clean;
- an unusable A1 snapshot;
- a run with a nonzero client exit or an unparsable report;
- a pilot abort;
- a missing run;
- fewer than 5 complete sessions.

**STOP** means a completed experiment broke a statistical or stationarity rule. It returns for methodology
review. No threshold is widened and no session is replaced. Causes:
1. a fixed or sentinel rep not sustained under `pilot.sustained()`;
2. a capacity rep with errors, stale or index_errors > 0 (EXEC overload is expected and not a failure);
3. any of CV_p99_between, CV_thr_between, any CV_p99,k or any CV_thr,k strictly above 10%;
4. the **supplementary** automatic multimodality signal: for p99 or for throughput, an adjacent gap of the
   sorted 5 session medians above 2 × the widest within-session range;
5. any session with **max(P_end/P_start, P_start/P_end) > 1.10** (item 1).

**STABLE** holds only if none of the above occurs:
- all 5 sessions and 60 runs complete;
- every anchor rep sustained;
- every capacity rep valid;
- every trim proven and every settle clean;
- every A1 snapshot usable and every guard passed;
- all CVs ≤ 10%;
- no multimodal signal;
- every sentinel ratio ≤ 1.10.

Then the A2-a start method is reproducible enough to proceed to the separately reviewed stable-peak
boundary validation. That does **not** freeze CRITERIA.

**Multimodality (item 7).** The gap rule is a supplementary automatic STOP signal. If it does not fire,
that **does not prove** the distribution is unimodal. The independent result review may still reject a
visibly or statefully clustered result even with every CV ≤ 10% and no automatic signal. The rule does
not change after data.

**Reported, not judged:**
- `anchor_median_of_session_medians_p99_ns` (historical anchor only);
- N_raw;
- per-rep p99.9, max and per-kind latencies; CPU and write bytes per op; memory per object;
- background-scan status and progress per snapshot; SSD temperature; host units started per session.

`a2a.py judge OUT` recomputes the verdict from `pilot.json`.

## Design points for the reviewer

1. **Which run failures are INCOMPLETE and which are STOP.**
   - A **nonzero client exit or an unparsable report is INCOMPLETE** (execution integrity, all run kinds).
     This goes slightly beyond item 5, which named the return code for capacity reps. It is applied to
     anchor reps too.
   - A capacity rep that **completed with errors, stale or index_errors > 0 is STOP** (a measured outcome).
     An **unsustained anchor rep is STOP**, as before.
2. **`sync -f /srv/snc-data`** runs before the pre-trim snapshot and again after the trim. It flushes any
   dirty ext4 metadata from clearing the data root, or from the trim itself, so a journal commit cannot
   fall inside the judged settle. It is not root and does not discard. Since revision 3, both calls
   fail closed (review 2).
3. **The guard's reference** is the boot id and HEAD observed right after the initial preflight, plus the
   frozen CP-1A hashes. Session 1 is guarded too.
4. **Host timers stay enabled.** `man-db`, `apt-daily` and others touch the boot disk. They are recorded,
   not masked.
5. **Unrelated to this package:** `test_pilot` has been red on `main` since #110 (`tests/test_pilot.py:366`).

## Validation before data (this commit)

- **Live, read only:** `observe()` and `guard_problems()` run against the host with the uncommitted
  revision in the tree. The only problem was `git tree not clean`, as expected with the edits uncommitted.
  Boot id, timer, NUMA, governors, mount, data disk and all 4 hashes matched. `sudo -n -l` was refused for
  all 4 commands, as expected before the sudoers install.
- `test_a2a.py` passes. It starts no process and trims nothing.
  - **A1:** constants, `SENTINEL_LIMIT` = 11/10, sudoers = `ROOT`, and the parsers and `a1_problems` on the
    real 2026-11-12 captures.
  - **A2:** each of 17 guard drifts is detected (16, plus `fstrim.service` active in revision 3).
  - **A3:** the exact per-session procedure sequence and commands. Each integrity failure the review lists
    stops at once with INCOMPLETE and nothing after it.
  - **A4:** every verdict boundary, including 1.10 passing, above it failing, both directions, and
    independence from N_raw.
- **Mutations:** `negative/summary.txt`, 47 mutants of `a2a.py`, all **caught on the first pass**. They
  include the review's required ones: the 1.10 edge, 1.05 and 1.15 limits, strict `<`, one-sided ratios
  both ways, the limit replaced by `1 + N_raw`, each guard field, the trim rc / bytes / delta / name, the
  settle write and discard, capacity rc / stale / index_errors / errors, and A1 not enforced. They also
  keep the revision-1 coverage.

## Commands (after final approval only)

```
sudo install -m 0440 bench/results/a2a-storage-repro-r640/sudoers-snc-a2a /etc/sudoers.d/snc-a2a
sudo visudo -c                       # result recorded in the evidence
cd /home/solo/SNC-server-OS          # HEAD = the approved preregistration merge, tree clean
PYTHONDONTWRITEBYTECODE=1 python3 -B bench/results/a2a-storage-repro-r640/test_a2a.py
python3 -B bench/results/a2a-storage-repro-r640/a2a.py run /home/solo/a2a-storage-repro-r640 \
    --scratch /dev/shm/snc-a2a --manifest bench/manifests/r640-main-20261111.txt --build build-s6
```
