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

## What is left

At 100% remote an op costs SNC about 2x a local op: two cache-line
messages (request and response), each written on one core and read on
another, plus the ring drain and dispatch. The baseline pays none of
that, because uniform keys almost never contend its stripe locks. The
profile left after this round has no single large item: the benchmark
driver and its per-op timestamps (also paid by the baseline), the op
itself, and the ring path in roughly equal parts. Going further needs
fewer messages per op, not cheaper ones: routing requests to the owner
at ingress (`docs/ROUTING.md`) and placing related objects together
(`create_near`, `docs/TRANSFERS.md`).
