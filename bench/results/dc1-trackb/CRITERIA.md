# Track B criteria: campaign dc1-trackb (B1, B2)

**Status: thresholds APPROVED, not yet FROZEN.**
- The thresholds and scope were approved by cody at P-G1, 2026-11-22 03:16 UTC
  (`docs/handoff/PR186_Owner_Decisions.md`). Their source is `docs/DIRECTION_COMPARISON_PLAN.md` revision 3, which Codex
  approved.
- The file is FROZEN by a later commit. That commit sets the open items below and records its SHA here. It lands
  before any comparative run.
- After the freeze, a change is a new commit, reported next to the results.
- No comparative number exists at the time of writing.

## Shared (plan 4.1)
| Item | Value |
|---|---|
| Host | R640, allocation PILOT-L, loopback, manifest captured first |
| Server cores | the same 8 physical node-0 cores for every system, hyperthread siblings idle |
| SNC profile | Profile A, LOCAL_DURABLE, one node, engine unchanged |
| Capacity at SLO | highest offered rate at which three confirmation runs each meet: p99 ≤ SLO, zero errors, application rejections ≤ 0.1% of operations, backlog within the limit, and the run drains |
| Bisection | 5% steps; a step runs 120 s and needs ≥ 600 probes where probes apply |
| Starting balance | 10^12 per account |
| Correctness | conservation check after two quiet rounds; a failed check makes the run invalid |
| Durability test | `dm-log-writes` over a loop device with `write_cache = write back` and `fua = 0`; fio positive and negative controls; 20 marks per configuration at about 25% of capacity; every candidate configuration tested before the R640 |
| R640 identity check | binary, config-file and command-line hashes of a verified configuration; a 10 s `blktrace` sample showing flush or FUA requests |
| PostgreSQL rule | a PASS against a PostgreSQL-gated benchmark is NARROW at most |
| Deciding boundary | full settlement only; debit acknowledgement reported only |
| Settlement probe | one durable read per probed transfer at intended send + SLO, reply due by + g, g = SLO/10; settled iff the balance ≥ expected balance (all earlier probe credits included) and, for PostgreSQL or Garnet commit steps, the issuer saw the commit return by the deadline; pass iff misses ≤ floor(0.01 n) |
| Probe budget | B = 0.2% of workload operations; f = B/x |
| Probe minimum | n_min = 3,000 per confirmation run |
| Window | T = max(120 s, n_min/(B·R)), capped at 600 s, the same T for every system at a point |
| Probe pool | max(1,024, probe rate × (SLO + g) × 10) accounts added to the 1M, never targeted by the workload |
| Low rate | below 2,500 ops/s, capacity is "below 2,500"; the verdict stands only if it holds over 0 to 2,500, else UNRESOLVED |
| Invalid runs | rerun once; twice invalid counts as failing; if the verdict would change had it passed, UNRESOLVED |
| Forbidden settings | Garnet `--aof-commit-freq` > 0 or no `--aof-commit-wait`; PostgreSQL `fsync=off`, `full_page_writes=off`, `synchronous_commit=off`; Restate `rocksdb-disable-wal-fsync` |
| Tuning budget | up to 12 documented configurations per baseline, reviewed by Codex before runs; one 30 s closed-loop screen each |

## B1: durable object execution (plan 4.2)
| Item | Value |
|---|---|
| Workload | 1M accounts, 16-byte keys, 200-byte values |
| Gated points | 0% transfers (50% reads, 50% deposits); 20% transfers (40/40/20, `--transfer 20 --reads 50`, `--xfer-local -1`) |
| Non-gating | Zipf 0.99 at 0% transfers |
| SLOs | reads and deposits p99 ≤ 1 ms; probed full settlement p99 ≤ 2 ms (g = 200 µs) |
| Gating order | Garnet `Native`, Garnet `FileStream`, PostgreSQL 18 |
| Diagnostic | `baseline_server` |
| PASS | ≥ 2.0x at 0% **and** ≥ 1.0x at 20% |
| FAIL | < 1.3x at 0% **or** < 0.8x at 20% |
| NARROW | otherwise |

## B2: durable messaging between entities (plan 4.3)
| Item | Value |
|---|---|
| Workload | 1M entities, all transfers, cross-shard (`--transfer 100 --xfer-local 0`); uniform targets gate; Zipf 0.99 reported only |
| SLO | probed full settlement p99 ≤ 10 ms (g = 1 ms) |
| Gating order | Restate (request-response ingress), PostgreSQL transactional outbox |
| Recovery | `SIGKILL` of every process at 80% of the system's own capacity after 120 s of load; time to serve again, and time until all settled |
| PASS | ≥ 3.0x **and** time to settle after recovery no slower than the baseline's |
| FAIL | < 1.5x **or** time to settle after recovery more than 2x slower |
| NARROW | otherwise |

## Open items, set by the freeze commit
- [ ] Warm-up length, and the drain bound.
- [ ] The absolute backlog limit (one number for every system), and the depth × threads values.
- [ ] The 8 server core ids and the client core ids.
- [ ] SNC's checkpoint interval for B2 recovery.
- [ ] The tuning configurations per baseline, after Codex's review.
- [ ] The PostgreSQL outbox example design (poll or NOTIFY, batch size, workers, `SKIP LOCKED`).
- [ ] Pinned versions and binary hashes for every system and client.
