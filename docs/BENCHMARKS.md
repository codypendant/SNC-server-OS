# v0.1 benchmark report

Step 12 of the handoff: does running operations on the shard that owns the
object beat the conventional design, on the same machine, with the same
objects, operations and durability rules? The report with charts is at
https://claude.ai/artifact/G2Xp7wtGDojTDwVMAXHnC8; this file keeps the method
and every number in the repository.

## Setup

- **Machine:** cloud VM, 4 vCPUs (Intel Xeon @ 2.80GHz), 1 NUMA node,
  Linux 6.18, ext4 on a virtio disk, GCC 13.3, RelWithDebInfo.
- **Data:** 1M Account objects (200-byte payload) plus the email index.
- **Load:** an in-process closed loop. Each worker (shard or thread) keeps
  `--depth` ops outstanding: 32 in memory, 256 durable. The driver shares
  the 4 cores with the engine.
- **Ops:** `get_balance` and `deposit` (`--reads` = share of reads).
- **Locality:** `--remote PCT` = share of ops whose target was created by,
  and in SNC is owned by, another worker. In the baseline all state is
  shared, so this changes only cache and lock locality.
- **Durable runs:** every op is DURABLE in both engines. Writes wait for
  their WAL record; reads wait until the state they saw is durable (D7).
  Same record format (56-byte header + after-image, CRC-32C), O_DIRECT,
  `RWF_DSYNC`, 50 µs or 64 KiB group commit, 4 KiB padding.
- **Runs:** 4 s per run, 3 runs per configuration, medians reported with
  the min–max of throughput. Raw runs: `bench/results/matrix.csv`.
  Re-run with `bench/run_matrix.py`, summarize with `bench/summarize.py`.

### The two engines

| | SNC (`snc_bench`) | Baseline (`baseline_bench`) |
| --- | --- | --- |
| Threads | 1 pinned shard per core | 1 pinned worker per core |
| Object map | shard-local open-addressing map, no locks | one shared map, 4096 `std::mutex` stripes |
| Remote op | message over SPSC ring to the owner | direct access under the stripe lock |
| WAL | one stream per shard, io_uring, no locks | 1 shared log (mutex + writer thread), or `--logs 4` partitioned logs |
| Email index | shard-local, broadcast lookup | shared, striped |
| Recovery | implemented (measured below) | not implemented |

The 4-log baseline is the stronger one: it removes the single log as the
bottleneck, as a well-built conventional engine would. Both baselines run
their log writers as extra threads on the same 4 cores.

## Results

| Group | Engine | Flags | ops/s (median, min–max) | p50 | p99 | p99.9 |
| --- | --- | --- | --- | --- | --- | --- |
| memory | snc | `--remote 0 --reads 50 --depth 32` | 5.16M (5.11–5.27) | 0.6 µs | 1.5 µs | 17.4 µs |
| memory | snc | `--remote 0 --reads 95 --depth 32` | 5.12M (5.09–5.45) | 0.6 µs | 1.6 µs | 18.9 µs |
| memory | snc | `--remote 50 --reads 50 --depth 32` | 4.06M (3.90–4.19) | 3.5 µs | 104.4 µs | 540.7 µs |
| memory | snc | `--remote 50 --reads 95 --depth 32` | 4.32M (4.09–4.63) | 3.3 µs | 94.2 µs | 466.9 µs |
| memory | snc | `--remote 100 --reads 50 --depth 32` | 3.64M (3.47–3.75) | 22.0 µs | 96.3 µs | 573.4 µs |
| memory | snc | `--remote 100 --reads 95 --depth 32` | 3.53M (3.47–4.01) | 22.5 µs | 100.4 µs | 639.0 µs |
| scaling | snc | `--shards 1 --remote 50 --reads 50 --depth 32` | 1.21M (1.20–1.24) | 0.7 µs | 1.7 µs | 20.5 µs |
| scaling | snc | `--shards 2 --remote 50 --reads 50 --depth 32` | 1.87M (1.87–1.90) | 21.0 µs | 86.0 µs | 254.0 µs |
| memory | baseline | `--remote 0 --reads 50 --depth 32` | 3.51M (3.49–3.57) | 1.0 µs | 1.9 µs | 23.6 µs |
| memory | baseline | `--remote 0 --reads 95 --depth 32` | 3.51M (3.42–3.54) | 1.0 µs | 1.9 µs | 24.6 µs |
| memory | baseline | `--remote 50 --reads 50 --depth 32` | 3.21M (3.19–3.29) | 1.1 µs | 2.0 µs | 25.1 µs |
| memory | baseline | `--remote 50 --reads 95 --depth 32` | 3.18M (3.06–3.32) | 1.1 µs | 2.0 µs | 25.6 µs |
| memory | baseline | `--remote 100 --reads 50 --depth 32` | 3.11M (2.98–3.34) | 1.1 µs | 2.0 µs | 26.1 µs |
| memory | baseline | `--remote 100 --reads 95 --depth 32` | 3.25M (3.24–3.37) | 1.1 µs | 2.0 µs | 24.6 µs |
| scaling | baseline | `--threads 1 --remote 50 --reads 50 --depth 32` | 0.76M (0.70–0.81) | 1.2 µs | 2.1 µs | 24.6 µs |
| scaling | baseline | `--threads 2 --remote 50 --reads 50 --depth 32` | 1.47M (1.45–1.47) | 1.2 µs | 2.3 µs | 25.6 µs |
| durable | snc | `--remote 0 --reads 50 --depth 256 --data-dir D` | 1.31M (1.18–1.33) | 426.0 µs | 5.11 ms | 8.65 ms |
| durable | baseline | `--remote 0 --reads 50 --depth 256 --data-dir D --logs 1` | 0.54M (0.53–0.55) | 1.05 ms | 3.60 ms | 17.30 ms |
| durable | baseline | `--remote 0 --reads 50 --depth 256 --data-dir D --logs 4` | 0.94M (0.91–0.99) | 753.7 µs | 3.47 ms | 6.68 ms |
| durable | snc | `--remote 50 --reads 50 --depth 256 --data-dir D` | 1.12M (1.03–1.18) | 720.9 µs | 5.64 ms | 9.44 ms |
| durable | baseline | `--remote 50 --reads 50 --depth 256 --data-dir D --logs 1` | 0.56M (0.55–0.59) | 1.05 ms | 3.28 ms | 6.03 ms |
| durable | baseline | `--remote 50 --reads 50 --depth 256 --data-dir D --logs 4` | 0.96M (0.96–1.00) | 737.3 µs | 3.08 ms | 9.18 ms |
| durable | snc | `--remote 100 --reads 50 --depth 256 --data-dir D` | 0.84M (0.83–0.93) | 852.0 µs | 6.03 ms | 13.89 ms |
| durable | baseline | `--remote 100 --reads 50 --depth 256 --data-dir D --logs 1` | 0.55M (0.54–0.56) | 1.05 ms | 3.21 ms | 6.95 ms |
| durable | baseline | `--remote 100 --reads 50 --depth 256 --data-dir D --logs 4` | 0.92M (0.92–1.06) | 704.5 µs | 3.60 ms | 6.95 ms |
| durable | snc | `--remote 0 --reads 0 --depth 256 --data-dir D` | 0.69M (0.68–0.74) | 1.08 ms | 2.69 ms | 5.64 ms |
| durable | baseline | `--remote 0 --reads 0 --depth 256 --data-dir D --logs 4` | 0.45M (0.42–0.51) | 1.93 ms | 4.72 ms | 17.30 ms |
| recovery | snc | `--remote 0 --reads 50 --depth 256 --data-dir D --no-checkpoint` | 1.11M (1.05–1.25) | 442.4 µs | 6.16 ms | 9.44 ms |

Other measurements (medians):

| Measurement | SNC | Baseline |
| --- | --- | --- |
| Resident memory per object after load (object, maps, index) | 483 B | 450 B |
| Checkpoint of 1M objects while serving (244 MiB) | 1.29–1.45 s | n/a |
| Restart from checkpoint + WAL tail | 0.84–0.94 s | n/a |
| Restart from WAL only (~2.6M records) | 2.54 s (2.18 s replay) | n/a |

## What the numbers say

1. **Locality pays when ops stay on their owner.** With every op local,
   SNC does 5.16M ops/s in memory against the baseline's 3.51M (1.47×),
   and 1.31M durable ops/s against the 4-log baseline's 0.94M (1.39×) and
   the single-log baseline's 0.54M (2.4×).
2. **Cross-shard traffic erodes the lead, and costs latency first.** In
   memory, SNC still has more throughput at 100% remote (3.64M vs 3.11M),
   but its median latency rises from 0.6 µs to 22 µs and p99 to about
   100 µs, while the baseline stays near 1 µs and 2 µs at any locality.
   Durable, SNC falls to 0.84M at 100% remote, below the 4-log baseline's
   0.92M.
3. **Durable tails are worse in SNC.** p99 is 5–6 ms for SNC against about
   3.5 ms for the baseline at the same throughput levels. (Follow-up in
   [ROUTING.md](ROUTING.md#durable-p99-the-cause): the cause is the
   kernel's io_uring workers starving for CPU behind 4 spinning shards on
   4 cores, not one flush in flight; with a free core p99 drops to about
   1.2 ms.)
4. **Scaling is near linear for both** from 1 to 4 cores (SNC 1.21M →
   4.06M at 50% remote, baseline 0.76M → 3.21M). SNC's 2-shard point
   (1.87M) shows the cost of the first remote messages.
5. **Memory is about 7% higher in SNC** (483 vs 450 B/object), mostly
   mimalloc's 320-byte size class for a 248-byte object plus the index.
6. **Recovery is fast enough for v0.1.** Under 1 s from a checkpoint and
   2.5 s from 2.6M WAL records, for 1M objects, after the parallel replay
   in this change (it was 3 s and 5.5 s).

So the v0.1 thesis holds for workloads that keep most ops on the owning
shard, and does not hold for uniformly random access: then SNC trades a
modest throughput gain in memory for 10–50× worse median latency, and
loses durable throughput. The next design question is placement: routing
requests to the owning shard (or placing related objects together) so the
remote share stays low.

## Limits of this comparison

- One small VM. The load driver, engine, and (in the baseline) log writer
  threads share 4 cores; NUMA effects and NIC-driven traffic are untested.
- The baseline is ours, written for this comparison, not a production
  database. It has no recovery, and it reads the clock and allocates the
  same way as SNC.
- Ops are tiny (one 200-byte object). Heavier handlers would shrink the
  share of time spent on dispatch and locking for both engines.
- Durable numbers depend on the virtual disk's sync latency.
