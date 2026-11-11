# S6 write-window causal characterization: preregistration

Written and committed **before any W1/W2 data**, under
[docs/handoff/PR104_S6_Result_Review.md](../../../docs/handoff/PR104_S6_Result_Review.md)
("NEXT AUTHORIZED TASK").

- **Not an S6 run.** It judges no S6 rule, and S6 stays **FAIL (rule 4
  p99.9)**.
- **Diagnostic only:** it is evidence for the review's choice of REFINE #1.
- **No product code change.**

## 1. Hypothesis (from the review)

S3's fixed checkpoint write window (W = 4 chunks in flight per shard)
saturates under P's light foreground load. The chunk writes become
bursty, WAL flush latency rises, and the durability-sensitive mutations
(deposit and transfer) get the p99.9 tail that failed rule 4. Walk pacing
cannot reduce I/O concurrency once the window drives storage pressure.

## 2. Cells

| Cell | Definition | Reps |
|---|---|---|
| W1 | exactly S6 P, plus `--ckpt-window 1` | 3 |
| W2 | exactly S6 P, plus `--ckpt-window 2` | 3 |
| W4 | **the frozen S6 P reps** (window 4, the default), read from `bench/results/s6-r640-20261111/raw/` | not rerun |

- **Order:** rep-major, W1 then W2 in each rep (W1-r0, W2-r0, W1-r1, …).
- **Identity:** `commands.txt` shows, and asserts, that every W server
  command is the S6 P command plus `--ckpt-window N`. The only other
  differences are the per-cell data directory, ports and ids-file name.
  Every W run command is P's.

**Unchanged from S6 P:**
- binaries: `snc_server` `69a51b69…`, `net_bench` `9fc54944…`; the
  pre-S1 hash is checked too;
- the R640, manifest e and storage state, checked by S6's preflight and
  postflight;
- PILOT-L CPUs and NUMA node 0; the sampler on CPU 0 and the disk
  observer on CPU 36;
- scratch `/dev/shm/snc-s6w` and output on the boot disk; data under
  `/srv/snc-data/s6w` (the same SSD as S6, a separate directory);
- 5,000,000 objects, the Profile-A mix, owner routing, 8 shards, 8
  threads;
- 436,646 ops/s open loop, depth 1,024, warm-up 10 s, window 60 s, drain
  10 s;
- checkpoint every 60 s, pacing 20/65/155/5 µs with batch 16, target
  30 s, hard maximum 60 s;
- 100 ms GAUGES/HEALTH sampling with no catch-up.

**Known confound, recorded before data:** W4 ran in the S6 session
(18:25-18:44 UTC); W1 and W2 run in a later session on the same machine
state. Rerunning W4 is not authorized ("do not rerun W4 merely to seek a
better result"). The comparison is therefore across sessions, and the
three-rep spread within each cell is the noise reference.

## 3. Runner and VOID policy

`wchar.py run` is used as is:
- it imports `bench/s6.py` (frozen at `63440b1`) unchanged and changes
  only the module state listed in its `setup()`;
- preflight refuses on any identity, isolation or clean-tree problem;
- postflight is the same as S6's;
- each W rep must prove a checkpoint began inside its measured window,
  with S6's rerun policy: a window or harness VOID is rerun once, at the
  end. A second window VOID, or a VOID again on the rerun, makes the
  session VOID, and it is reported as such;
- a rep with a failing metric is a result and is never rerun.

The characterization runs **once**.

## 4. Recorded per rep (`char.json`, from the raw files)

- **Client:**
  - total and per-kind (read, deposit, transfer, index) p99, p99.9 and
    max;
  - EXEC and INDEX refusals;
  - `late_over_1ms` / `scheduled_in_window`;
  - errors, stale, index errors, drained, client and server rc.
- **WAL and controller (from the 100 ms samples):**
  - WAL EWMA peak and time above WAL-high;
  - backlog peaks and time above 25% and 50%;
  - walk-budget range; catch-up time;
  - failed samples and skipped deadlines.
- **Checkpoint write path, per shard (final GAUGES):**
  - `ckpt_write_lat_ewma_us`, `ckpt_write_lat_max_us`;
  - `ckpt_window_full_steps`, `ckpt_chunks`, `ckpt_window`;
  - `ckpt_last_snapshot_us`, `ckpt_last_walk_us`, `ckpt_last_total_us`;
  - `ckpt_catchup_steps`, `ckpt_step_max_ns`, `emits_peak`,
    `wal_flush_ewma_us`, `wal_flush_max_us`.
- **Machine:** pre and post identity, as S6.

W4's figures come from the S6 raw files through the same extraction
(`wchar.py w4`).

## 5. Interpretation, frozen before data (the review's text)

The purpose is causal discrimination, not choosing a favourable PASS
threshold.

**Supports the fixed-window hypothesis** if lowering W produces a
coherent reduction in all of:
1. checkpoint write latency;
2. WAL EWMA peak and time above WAL-high;
3. mutation (deposit and transfer) tail latency;

while checkpoint completion stays comfortably inside the 60 s hard
maximum. The evidence should be monotonic, or at least directionally
coherent, W1 ≤ W2 ≤ W4 for the storage-pressure and tail metrics,
allowing normal measurement noise.

**Does not support it:**
- If W1/W2 materially reduce checkpoint write latency but mutation p99.9
  stays near the W4 failure, the write window is not sufficient to
  explain the tail.
- If neither write latency nor WAL pressure materially changes, the
  hypothesis is rejected.
- If checkpoint duration becomes pathological, that is recorded. The hard
  maximum is not widened.

No W value is selected here. The result returns for independent review,
which chooses REFINE #1 (window control), S5, B2 or another bounded
hypothesis.
