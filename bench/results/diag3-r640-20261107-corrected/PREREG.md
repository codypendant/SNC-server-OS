# Corrected B/T non-perturbation proof and causal cells: preregistration

*Status: **FROZEN for review**, written and committed before any data.
Authorized design: `docs/handoff/PR62_Codex_Review.txt` and
`docs/handoff/PR64_Reconciliation_Review.txt` (on main). **No R640 load is
authorized yet.** After review, the next authorization is for the
corrected B/T proof. If G0-G4 all pass, the causal cells below follow
with no further approval. If any gate fails, the driver stops.*

Driver: `diag3.py` in this directory. Tests: `test_diag3.py`. Software
validation: `VALIDATION.md`.

## Why a new proof

R640 step 1 (`../step1-r640-20261107-ids-shm/`) confirmed a **benchmark
defect**. The 80 MB account-ID file was written buffered onto the data SSD
just before each run, and its writeback disturbed the WAL. The old proof
(`../diag2-r640-20261106-telemetry/`) ran with that defect in both arms.
It is kept as historical evidence. **None of its values are reused** for
any gate here.

## Benchmark scratch isolation (both arms, identical)

The rule is the one approved for the corrected CP-1A harness
(`bench/pilot.py`, `docs/PILOT_PLAN.md` section 8; PR #64):

- `--scratch DIR` is mandatory. Every ID file of both arms goes there, as
  `<scratch>/<arm>-<kind>-r<rep>.ids`. Each file is deleted after its run.
- Neither the scratch directory nor the output directory may share a
  physical disk with the data directory `/srv/snc-data/diag3`.
  - Partitions resolve to their disk, device-mapper and md devices to
    their members.
  - A memory filesystem counts as the disks its swap is on.
  - A disk that cannot be resolved (btrfs, overlay, loop, network, FUSE)
    means refusal.
- The data directory must resolve to exactly one disk, because the
  observer reads that disk.
- The scratch directory needs at least twice the ID file's size free.
- The paths and resolved disks go in `preflight.json`.

On the R640, the planned layout is:
- `--scratch /dev/shm/snc-diag3`, acceptable only if the resolver shows
  that swap is not on the data SSD;
- `--out` in the repository on the BOSS disk.

`/dev/shm` itself is not part of the method. The isolation is.

**B and T differ only by diagnostic telemetry.** That means the build
directory and T's `--diag-gauges`/`--diag-cpu`, `--diag-buckets` and
100 ms disk observer. Scratch placement, data directory, CPUs, binding,
objects, mix, route, depths, warm-up, window, drain and checkpoint
interval are identical. `test_diag3.py` checks every cell for this.

## Arms

- **B:** `build-pilot`, the pilot's binaries. `snc_server` sha256
  `04030ccb...3226` and `net_bench` sha256 `a5f085d4...3863`; the driver
  checks the full hashes.
- **T:** `build-diag`, the telemetry build. Its hashes are recorded at
  preflight and must be unchanged at the end.
- Allocation is PILOT-L from `bench/pilot_plan.json`:
  - server on CPUs 2-16 even;
  - client on CPUs 18-32 even;
  - memory on node 0;
  - gauges sampler on CPU 0;
  - disk observer on CPU 36.
- Workload: 5,000,000 objects, 8 shards, mix
  `read=70,deposit=15,transfer=10,index=5`, route owner.

## Proof cells

There are 3 reps. Within each rep the order is B then T, and each arm
runs closed then w30.

| Kind | Load | Window | Checkpoint |
|---|---|---|---|
| closed | closed loop, depth 512 | 60 s | every 60 s |
| w30 | 2,183,233 ops/s open loop, depth 1024, warm-up 10 s, drain 10 s | 30 s | every 60 s |

## Gates

All use medians over the 3 reps and exact rational arithmetic. A missing
rep fails every gate.

**G0, machine and baseline reproducibility.** It does not test zero
refusals; the old zero-refusal clause is removed. G0 passes only if all
three parts hold:
- **A.** The median B closed throughput is within 5% of 4,366,466 ops/s.
- **B.** The median B w30 total p99 is at most 1.5 × 1,048,576 ns
  (the original 1.05 ms reference, a histogram bucket edge), so at most
  1,572,864 ns.
- **C.** The identity checks hold at preflight and again after the proof:
  - kernel, CPU count, CPU model, node 0 CPU list, and MemTotal within
    0.1% all match the approved manifest d;
  - all CPU governors are `performance`;
  - `kernel.numa_balancing` = 0;
  - the PILOT-L CPUs exclude the housekeeping CPUs;
  - the B binaries' hashes match;
  - the data directory's disk is model `ARFX3840S5xnFTRI`, with
    mq-deadline and write back;
  - the T binaries are unchanged during the proof.

  If C fails at preflight, the driver refuses before any run.

**G1 (unchanged):** the median T closed throughput is at least 0.97 ×
the median B closed throughput.

**G2, telemetry non-perturbation of refusals.** For each w30 rep:

`refusal_fraction = (EXEC refusals + INDEX refusals) / scheduled_in_window`

The refusal counts are `net_bench`'s whole-run counts (warm-up, window
and drain), which is the stricter reading. G2 passes only if both hold:
1. median(T refusal_fraction) ≤ median(B refusal_fraction) + 0.0001;
2. every T rep's refusal_fraction ≤ max(B refusal_fraction) + 0.0002.

These are diagnostic tolerances, not campaign performance or materiality
thresholds.

**G3 (unchanged):** the median T w30 p99 and p99.9 are each at most
max(1.25 × B median, B max).

**G4 (unchanged):** the median T w30 throughput is at least 0.995 × the
median B w30 throughput.

The gates and their parts are written to `gates.json`.

## Causal cells

These are T only, 3 reps, in the order below within each rep. They run
only if G0-G4 all pass:

| Cell | Rate | Window | Checkpoint |
|---|---|---|---|
| r436646-ckpt | 436,646 | 60 s | every 60 s |
| r436646-nockpt | 436,646 | 60 s | none (the server's default is never) |
| r2183233-w60 | 2,183,233 | 60 s | every 60 s |

The T-arm closed and w30 data come from the proof.

## Run discipline

- Every run happens once. There are no reruns, and no changes after a
  result. Any failure stops the driver.
- The driver refuses if the output directory already exists, if the git
  tree is not clean, or if `snc_server` or `net_bench` is already
  running.
- The driver records the git HEAD. The R640 runs it from the frozen
  commit named in the review handoff.
- If any gate fails: STOP, commit the evidence, and return for review.
  The gate is not reinterpreted or weakened after the results are seen.

## Not changed, not authorized

- The CP-1A campaign criteria, f = 0.70, the grid, the workload and the
  pilot plan values are unchanged.
- Nothing in the engine, `kEmitBacklog`, checkpointing, the WAL I/O
  policy, the SSD or the filesystem changes.
- No full pilot rerun, no competitors, no Profile C.
- `r640-diag-telemetry` stays off main.
