# Cross-core handoff cost

In-process ops that land on another shard were SNC's weakest result: at
100% remote, SNC ran at 0.60x the locked baseline (7.22M vs 12.05M ops/s,
`bench/results/matrix_msg64.csv`). This round profiles that path and
removes what costs time without doing work.

## Changes

| Change | Why | Effect (A/B, in process, remote100) |
| --- | --- | --- |
| `snc_hash64` compiles xxHash inline (`XXH_INLINE_ALL` in its one .cpp) | Every ID is hashed 2-4 times per remote op; each was a call into the shared library plus its length dispatch. Same function, same values (checked against the library on 1M IDs), so slots and data files are unchanged. | 6.7M -> 7.5M; local ops 15.2M -> 15.8M |
| `sri_claim` / `sri_commit`: requests, responses and follow-ups are written straight into the ring slot | Building a message on the stack field by field and copying it into the ring made the copy's wide loads wait on store forwarding. | about +4% |
| `publish_all` fences once per loop iteration instead of once per destination | The fence that pairs with a sleeping consumer also waits for the store buffer, which holds the stores to rings other cores own. | about +2% |

Tried and dropped: a write prefetch of ring slots ahead of the producer
(no change), and fixed-size copies of inline payloads instead of
`memcpy` (no change, within noise).

## Results

`bench/results/matrix_crosscore.csv`, 4-vCPU VM, 1M accounts, mean of 3
runs. The VM ran slower overall than for the previous matrix (the
unchanged baseline lost 3-15%), so the ratio within one matrix is the
fair comparison:

| In process, memory | SNC | Baseline | Ratio now | Ratio before |
| --- | --- | --- | --- | --- |
| 0% remote, 50% reads | 15.62M | 13.09M | 1.19x | 1.09x |
| 50% remote, 50% reads | 9.37M | 11.45M | 0.82x | 0.67x |
| 100% remote, 50% reads | 7.80M | 11.63M | 0.67x | 0.60x |
| 100% remote, 95% reads | 8.48M | 14.06M | 0.60x | 0.49x |

Durable, SNC stays ahead of the 4-log baseline: 2.04-2.07M vs 1.07-1.34M
ops/s with 50% reads, 0.97M vs 0.65M deposits only.

## Batched ring messages

Requests one shard sends another within one loop iteration now travel as
one `MULTI_REQ` message: up to 32 48-byte records (target, op, pending
slot, flags, args up to 16 bytes) in a buffer from the sender's heap. The
owner prefetches all the targets, runs the ops, writes each response over
its record and returns the same buffer as `MULTI_RESP`; the sender
completes the ops and frees it. A batch of one is sent as an ordinary
request. An op that must wait for durability, or whose result is over 16
bytes, is answered with its own `RESP` and marked in the batch as
answered apart. Requests with a version condition or larger args are not
batched. `Engine::Config::batch_remote` (on by default; `snc_bench
--no-batch`) turns it off.

Messages per remote op fall from 2.0 to about 0.5. What that buys depends
on how many requests are in flight, because a shard batches what it
issued in one loop iteration. The owner also publishes each batched
response as soon as it is written (`sri_publish`) instead of at the end
of its loop iteration. In process, 100% remote, 50% reads, mean of 3 runs
(`bench/results/matrix_batch_depth.csv`):

| Requests in flight per shard | 32 | 128 | 512 |
| --- | --- | --- | --- |
| SNC, batched | 9.31M | 11.43M | 12.47M |
| SNC, one message per op | 8.04M | 9.36M | 10.05M |
| Baseline | 11.32M | 11.84M | 10.42M |
| SNC / baseline | 0.82x | 0.97x | 1.20x |

At depth 32 batches stay small (about five ops per destination per
iteration), so each op carries a larger share of the loop's fixed work:
draining every ring, publishing, and one prefetch pass per batch. The
shards are not waiting: counting cycles spent in loop iterations that found
no work gives under 1% at depth 32. Deeper pipelines (an ingress with many
clients, or a batch job) fill the batches, and at 512 in flight SNC
overtakes the baseline on all-remote work.

The cost is latency: the baseline answers in 0.2 us at p50, because it
runs the op on the calling thread; SNC's remote p50 is 8.5 us at depth 32
and 104 us at depth 512 (179 us without batching). Ops that reach their
owner directly (TCP routing, `docs/ROUTING.md`) do not pay it.

Durable throughput is unchanged (`durable` group in `matrix_batch.csv`: 1.86-2.03M
ops/s against 0.91-1.21M for the baseline), since durable writes are
answered apart once their record is on disk.

## What is left

At shallow depth the remote path is bound by work per op, not by waiting:
the shards spend under 1% of their cycles in idle iterations, and the
latency follows from throughput (32 ops in flight at about 2.3M ops/s per
shard is about 14 us each). Sending a batch earlier or polling more often
would not help, since nothing waits.

Tried and dropped: one drain pass over every inbound ring, prefetching the
batch buffers and targets of all sources together before running any.
It moved depth 32 by +5% and depth 512 by -4% in alternating A/B runs,
both within the noise of this VM, so the per-ring prefetch stays.

Ops that reach their owner directly avoid the path entirely, which is why
routing at ingress (`docs/ROUTING.md`) and placing related objects together
(`create_near`, `docs/TRANSFERS.md`) matter most.
