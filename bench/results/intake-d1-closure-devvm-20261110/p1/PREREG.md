# P1 preregistration: follow-up intake D.1 regression matrix (dev VM)

Written and committed before the run. Ordered by the PR #87 review
(docs/handoff/PR87_D1_Review.txt): "Run the preregistered P1 regression
matrix: W={16,64,256}, X={256,1024,4096}, plus rollback/no-window. Report
overhead; do not tune production defaults from the dev VM."

- **Host:** this dev VM (4 vCPU, `manifest.txt`). A regression check
  only. No conclusion is drawn about any target machine, and no default
  (W = 64, X = 1024, H, L) changes on the strength of it.
- **Build:** Release, the closure-PR head (commit named in `summary.txt`
  and `runs.log`).
- **Tool:** `bench/p1_intake.py build OUT --reps 5 --seconds 8 --shards 2
  --objects 100000`. snc_bench runs in process with a durable WAL
  (O_DIRECT, RWF_DSYNC), depth 32 per shard, and a fresh data directory
  per run that is removed afterwards.
- **Arms:** the nine cells w{16,64,256} × x{256,1024,4096} (H and L
  default), no-window (W = 0, X = 1024), and rollback
  (`SNC_FOLLOWUP_INTAKE=off`: H, W, X, owner gate and L off; the contract
  checks stay on).
- **Workloads:** uniform (100% transfers to a uniform random account) and
  cross (100% transfers, all to another shard's account: one exchange per
  transfer).
- **Order:** every rep runs all 22 cells once, in a seeded random order.
- **Reported per cell:**
  - ops/s median, min and max over the valid reps;
  - the ratio of that median to rollback's median for the same workload
    (the overhead);
  - the median p99 latency;
  - the median number of turned-away (E_OVERLOADED) ops.
- **A run is valid** when rc = 0 and errors = 0. Every invalid run is
  listed, none is dropped silently, and the matrix is not re-run to
  replace one.
- **Interpretation rule:**
  - The overhead is the ratio to rollback.
  - A dev VM's run-to-run spread is often 5 to 10%, so a median ratio
    within the rollback arm's own min..max range is reported as "not
    distinguishable from rollback".
  - A cell whose median falls below rollback's min is reported as an
    overhead, with its size.
  - Turned-away ops are reported alongside the ratio, because a refused
    op is not counted in ops/s.
