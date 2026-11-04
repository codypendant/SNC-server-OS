# S1 evidence, 2026-11-04

These are the raw results behind MIGRATION_STAGING.md section 18.4.

## Host and code

- **Host:** this project's cloud VM. Its manifest is
  [bench/manifests/dev-vm-4vcpu-20261104.txt](../../manifests/dev-vm-4vcpu-20261104.txt):
  4 vCPU, 15 GiB, loopback.
  - Every node of the cluster ran on this one host: 3 witnesses, 4
    servers with 2 shards each, the driver and the probes.
  - The host is therefore CPU-oversubscribed. These numbers are
    regression evidence, not experiment evidence (EXPERIMENTAL_VALIDATION.md
    section 7).
- **Code:** the S1 commit of the PR that adds this directory, on base
  `3c3241d`.
  - The build (`build/`, RelWithDebInfo) was linked at 21:41 UTC; no
    source changed after 21:42.
  - The runs ran from 21:44 to 21:58 UTC.

## Files

- `raw/m_{on,off}_{shared,unique}_{1,2,3}.json`: the matrix, one file per
  `bench/s1_move.py` run.
- `raw/sw_*.json`: the budget sweep (2 runs each):
  - `us250` and `us4000`: unit time 0.25 ms and 4 ms;
  - `by65536` and `by4194304`: unit bytes 64 KiB and 4 MiB;
  - `wb65536`: window 64 KiB.
- `summary.txt`: `python3 summarize.py` over `raw/`, showing the median
  with the minimum and maximum.
- `matrix.sh`: the exact commands. Every run uses the defaults of
  `bench/s1_move.py`:
  - 100,000 objects in one slot;
  - `--chunk 256`;
  - 4 writers over 18,000 accounts;
  - unit 1000 µs, 1 MiB, window 8 MiB;
  - `SNC_TRACE=1`.

  The only changes are the flags each line names.

## Reproduce

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && ninja -C build
    OUT=/some/dir bash bench/results/s1-20261104/matrix.sh

Each JSON's `work` field names the run's temporary directory, which has
since been deleted.
