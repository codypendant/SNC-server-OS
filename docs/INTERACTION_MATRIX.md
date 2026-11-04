# Interaction matrix (deliverables C and M)

Faults and features are each tested alone; most incidents come from two
at once. This matrix lists the pairs that touch the core guarantees of
[FAILURE_MODEL.md](FAILURE_MODEL.md) (G1 single owner, G2 durable ack,
G3 at-most-once, G4 obligations settle, G5 recovery, G6 liveness), what
the code does, and what evidence exists.

Labels as in [STABILIZATION_REPORT.md](STABILIZATION_REPORT.md):
OBSERVED, MEASURED, INFERRED (code reading, EL0), UNPROVEN. Evidence
levels are EL0-EL6.

Class, per work order Part IX:

| Class | Meaning |
| --- | --- |
| **TESTED** | a test or run exercises the pair and checks the guarantee |
| **PARTIALLY TESTED** | exercised, but not every case, or the guarantee is not checked |
| **UNTESTED** | nothing exercises it |
| **KNOWN ISSUE** | evidence of a violation |
| **NOT APPLICABLE** | the pair cannot occur |

## C. The matrix

### Retry and sessions

| # | A × B | What happens (as built) | At risk | Evidence | Class |
| --- | --- | --- | --- | --- | --- |
| 1 | Retry × overload | The duplicate check runs before `E_OVERLOADED` [INV 18] | G3 | `test_transfer` `test_resend_overloaded` (EL2), found on real hosts | **TESTED** |
| 2 | Retry × migration (resend to the source) | Deduplicated before `E_MOVED`/`E_MIGRATING`, also after a restart | G3 | `test_resend_refusals` (EL2) | **TESTED** |
| 3 | Retry × migration (resend to the new owner after `E_MOVED`) | Sessions stay on the source; the destination has none, so a resend of an applied-but-unanswered request runs again. Refined (EXPERIMENTAL_BASELINE.md Q2): the shipped library resends an in-doubt request to the same group, which deduplicates before `E_MOVED`, so the double apply needs an expired (R11) or lost (R08) source session, or a resend that bypasses the library | G3 | INFERRED | **UNTESTED** (pre-baseline test T-Q2) |
| 4 | Retry × failover (sync) | Sessions are replicated; resends after failover are deduplicated | G3 | `test_replication` 30K resends (EL3), Vultr P3 (EL4) | **TESTED** |
| 5 | Retry × second failover during resends | Same path as row 4, two terms | G3 | `RETRIES.md:156-161` | **UNTESTED** |
| 6 | Retry × recovery (restart) | Sessions are WAL objects, recovered with the data | G3 | `test_resend_refusals` (restart while sealed), `test_crash` (EL3, acked results; no resends) | **PARTIALLY TESTED** |
| 7 | Session expiry × retry | The resend finds no session and runs again | G3 | `test_session_ttl` (EL2, MEASURED) | **KNOWN ISSUE** (R11) |
| 8 | Shard-count change × sessions | Session records are rerouted by their own ID; one shard can hold several sessions per client; `find_session` reads only the first | G3 | INFERRED; guarded only by the client refusing another shard count | **UNTESTED** |
| 9 | Checkpoint × sessions | Expiry runs at checkpoint start; checkpointed sessions recover | G3 | `test_session_ttl` (expiry), `test_persistence` (checkpoint, no sessions checked) | **PARTIALLY TESTED** |

### Replication and HA

| # | A × B | What happens (as built) | At risk | Evidence | Class |
| --- | --- | --- | --- | --- | --- |
| 10 | Async replication × witness promotion | The witness promotes an async standby; acknowledged writes are lost | G2 | `test_async_failover` 500/500 (EL2) | **KNOWN ISSUE** (R08) |
| 11 | Long shard task × witness health | Before HEALTH: `STATUS` waited on every shard thread, the witness poll timed out at 100 ms, promotion after 1 s, the promoted node had no replica. With every witness on `HEALTH` (HEALTH_DECOUPLING.md section 10): no promotion; the stalled shard's clients wait (W1; for the final stage, at most one unit after S1) | G6, then G2 (unreported LOCAL_DURABLE), before the fix | `test_health.py` T1 (3 s stall), T3 (6 s), T2 (A/B: STATUS witnesses still promote), T10 (one STATUS witness still promotes), T12 lab P5 100K move traced (no promotion, 4.8 s final stage) | **TESTED** (EL3), when every witness polls `HEALTH`; **KNOWN ISSUE** with any `STATUS` witness |
| 12 | Standby seeding × primary death | A seeding node is still in `Engine::start`, so it never answers `STATUS` and cannot be promoted (CORE_CONTRACTS E6) | G2 | INFERRED (EL0) | **UNTESTED**, settled at EL0 |
| 13 | Checkpoint × lost standby with a witness | The main thread waits for durability that needs the standby, and is also the thread that asks for DEGRADE | G6 (wedged primary) | INFERRED | **UNTESTED** |
| 14 | Sync blocked (`repl_blocked_`) × client load | Waiters, held follow-ups, pipelining and must-deliver overflow grow without limit until the grant | G6, then memory | INFERRED | **UNTESTED** |
| 15 | Network flap × failover | Each silence over `failover_ms` can fail over; the fenced old primary stays out until `--rejoin`; no hysteresis | G6, then G2 | INFERRED | **UNTESTED** |
| 16 | Async forced promotion × sessions | Sessions are as old as the standby; earlier OKs may be lost, resends may apply | G2, G3 (as designed under F-4) | INFERRED | **UNTESTED** |
| 17 | Clock jump × standby read lease | Each side's own clock with a 0.9 factor | standby read consistency | INFERRED: safe only with bounded rate drift | **UNTESTED** |
| 18 | Replication × recovery (restart of either member) | The primary resumes shipping from the standby's acked LSN; the standby truncates and recovers like a primary | G2 | `test_replication` (EL2-EL3) | **TESTED** |
| 19a | Partition × reads on the old primary | A cut-off sync primary answers default reads of already-durable objects until it learns the new term | single-object read consistency | INFERRED (EL0) | **UNTESTED** (KNOWN GAP) |
| 19b | WAL bit rot × recovery | A bad record mid-way in the last segment drops the acknowledged records after it silently | G2 | INFERRED (EL0) | **UNTESTED** (KNOWN GAP) |
| 19c | Restart with a shard-count change × lost standby with a witness | The step-8 recovery checkpoint waits for the standby inside `Engine::start`, before DEGRADE can be asked | G6 (restart hangs) | INFERRED (EL0) | **UNTESTED** |
| 19 | Control-plane (witness majority) loss × failover | No promotion and no DEGRADE grant until a majority returns; INFERRED: serving continues, a sync primary that loses its standby blocks | G6 | INFERRED | **UNTESTED** |

### Migration and placement

| # | A × B | What happens (as built) | At risk | Evidence | Class |
| --- | --- | --- | --- | --- | --- |
| 20 | Migration × failover (destination or source group fails over mid-move) | Slot state is a replicated object; the promoted node runs the start barrier (placement v2: arrival proof, re-seal, commit gate) and resumes the node-side steps; a volatile S1 chain is lost and the driver's resend starts a new one | G1, G4, G6 | `test_migrate` (sync groups, standby promoted at each step, EL3); `test_fail_closed` F1, F3, F6, F7, F13, F17 (lossy async promotion, EL2, PR #40); `test_s1` dst_promote, src_promote, fence (EL2, PR #44) | **TESTED** |
| 21 | Migration × replication | Moves run between sync-replicated groups; staged objects and seals are replicated | G2 | P5 lab (EL2, loopback), Vultr P5 (EL4) | **PARTIALLY TESTED** (no standby check after a move) |
| 22 | Migration × recovery (node crash at each step) | Every step resumes from durable slot state | G1, G4 | `test_migrate` 67 crash points (EL3); `test_s1` crashes inside the final stage chain at admit, check, put, scan, stage and reply, one restarting with units off (EL2) | **TESTED** |
| 23 | Migration × follow-ups (transfers into and out of a moving slot) | Follow-ups to a sealed slot are held and re-emitted after activation | G4 | `test_migrate` (transfers during moves, all settle) (EL3) | **TESTED** |
| 24 | Migration × silent partition | Stale seal answered; the PR #28 double apply fixed | G3 | Vultr rerun 24/24 (EL4) | **TESTED** |
| 25 | Placement × recovery (restart with an older map) | `E_MIGRATING` until refreshed | G1 | `test_resend_refusals` (EL2) | **TESTED** |
| 26 | Control-plane loss × migration | The driver stops; the slot can stay sealed until a majority returns; resumable | G6 | driver death tested (EL3, and mid-chain: `test_s1` driver_death); an offline restart without the control plane does not start (F17); witness-majority loss mid-move not tested | **PARTIALLY TESTED** |
| 27 | Checkpoint × migration (seal or staged objects across a checkpoint and restart) | Slot state is an object; checkpointed with the rest | G1 | INFERRED | **UNTESTED** |
| 28 | Indexes × migration | Results filtered to served slots; staged objects re-indexed on `put_image` (the D amplifier) | index results; G6 via D | MEASURED (D) for cost; correctness INFERRED | **PARTIALLY TESTED** |

### Recovery, obligations, resources

| # | A × B | What happens (as built) | At risk | Evidence | Class |
| --- | --- | --- | --- | --- | --- |
| 29 | Failover × follow-ups in flight | Outboxes are replicated; the new primary resumes them after its follow-up barrier | G4 | `test_cluster` (EL3) | **TESTED** |
| 30 | Follow-ups × overload | The backlog refuses new client work; follow-ups are never refused, only queued | G4, G6 | Vultr P4: 26K backlog settled after a cut (EL4) | **PARTIALLY TESTED** |
| 31 | Recovery × node links and replication shipping | Both start before the barrier; follow-ups are held | G5 | follow-ups held (EL2 `test_transfer`); requests over links before `serving` INFERRED | **PARTIALLY TESTED** |
| 32 | Wrong data directory × control plane | The space-UUID check runs after the node serves | G1 (briefly) | INFERRED | **UNTESTED** |
| 33 | Promotion × index lookup | Shards are promoted in parallel and finish at different times, with no client barrier | index results | INFERRED | **UNTESTED** |
| 34 | Target inbox full × fan-in | `kSettleRetry` re-emits at once, no backoff | G6 (hot loop, WAL growth) | INFERRED | **UNTESTED** |
| 35 | Follow-up dropped for an unexpected status × no restart | Re-emitted only on restart, link up, map change, unseal or activate | G4 (unbounded delay) | INFERRED | **UNTESTED** |
| 36 | Memory exhaustion × first write of a new client | `record_session` aborts after `apply` changed the object | G6 (failover recovers) | INFERRED | **UNTESTED** |
| 37 | Disk full or EIO × WAL | Fail-stop abort | G2 holds; G6 by failover | INFERRED | **UNTESTED** |
| 38 | Client routing × failover | `net::Client` fails over on `E_STANDBY`/`E_FENCED` and resends every unanswered request | G3 | `test_replication`, `test_net` (EL2-EL3) | **TESTED** |
| 39 | Client routing × migration | `ClusterClient` refreshes on a newer epoch; `E_MIGRATING` is returned to the caller | G6 | `test_migrate` client (EL3) | **TESTED** |

Not applicable: a standby × migration-driver requests (the driver talks
only to primaries); in-memory mode × replication.

## Baseline triage (2026-11-04)

[EXPERIMENTAL_BASELINE.md](EXPERIMENTAL_BASELINE.md) section 3 classifies
every row that matters to the falsification campaign as BLOCKER,
EXCLUSION, TEST REQUIRED or NON-BLOCKING per benchmark profile, and
section 5 names the pre-baseline tests (T-Q1..T-Q10, T-H). Row 20 is now
TESTED and leaves the ranking below.

## M. Untested high-risk interactions, ranked

Ranked by safety impact first: rows that can serve from two owners (G1),
lose an acknowledged write (G2) or apply twice (G3) come before rows
that can only stall (G6). Within a band, by how likely a real deployment
meets it. Known issues (rows 7, 10, 11) are not ranked: they are
measured, with contracts proposed in CORE_CONTRACTS sections 9-11.

| Rank | Row | Impact | Why this rank | Smallest test that would settle it |
| --- | --- | --- | --- | --- |
| 2 | 3: resend to the new owner after `E_MOVED` | G3 | Needs a lost answer plus an expired or lost source session, or a client that bypasses the library (Q2) | `test_resend_refusals`: apply, drop the answer, move the slot, resend to the destination |
| 3 | 32: wrong data directory before the UUID check | G1 | Operator error, brief window, but two owners | start a node on another group's directory with `--control`; expect refusal before serving |
| 4 | 19b: WAL bit rot in the last segment | G2 | Silent loss of acknowledged writes on one node; a sync standby still holds them | `test_persistence`: flip a byte mid-segment, restart; expect a refusal, not a silent truncation |
| 4b | 19a: stale reads on a cut-off old primary | consistency | Any partition; reads only | `test_replication`: cut the primary from standby and witnesses, promote, write on the new primary, read on the old |
| 4c | 12: standby seeding when the primary dies | G2 | Settled at EL0 (a seeding node cannot answer `STATUS`); a test would confirm | `test_witness.py`: kill the primary while the standby seeds; expect no promotion |
| 5 | 16: async forced promotion × sessions | G2, G3 | Becomes the designed path once F lands | part of the F tests (CORE_CONTRACTS section 9) |
| 6 | 5: second failover during resends | G3 | Same path as row 4; INFERRED safe, never run | `test_replication` with two consecutive kills during a resend storm |
| 7 | 8: shard-count change × sessions | G3 | Silent if a client bypasses the refusal | `test_persistence`: sessions on 4 shards, restart on 2, resend |
| 8 | 27: checkpoint × migration | G1 | Narrow window; slot state is an ordinary object | `test_migrate` with `checkpoint()` between seal and activate, then restart |
| 9 | 13, 19c: checkpoint × lost standby with a witness, at run time and at restart | G6 | A wedged main thread (it also asks for DEGRADE). `HEALTH` keeps answering and only `main_ms` shows it (HEALTH_DECOUPLING.md HF4); no witness acts on it | `test_witness.py` `--checkpoint-s 1`, kill the standby mid-checkpoint |
| 10 | 15: flapping links | G6, then G2 | Each flap can leave a group without a replica | lab `link` fault on a 1.5 s period for a minute |
| 11 | 14: blocked sync × load | G6, memory | No bound | standby SIGSTOP plus a witness denying DEGRADE; watch RSS |
| 12 | 19, 26: witness majority loss × failover / migration | G6 | Rare; by design no decisions without a majority | kill 2 of 3 witnesses during a move and during a primary kill |
| 13 | 35, 34: obligation stalls, hot retry | G4 delay, G6 | No loss | injected unexpected status; fan-in to a full inbox |
| 14 | 33, 17, 36, 37 | index results, standby reads, fail-stop | Fail-stop by design or weaker read classes | STABILIZATION_REPORT P |
