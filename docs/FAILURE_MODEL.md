# Failure model (deliverable B)

Which faults SNC is meant to tolerate, what it guarantees under each, and
how strong the evidence is. The guarantee names are defined in
[CORE_CONTRACTS.md](CORE_CONTRACTS.md); the evidence levels are those of
[STABILIZATION_REPORT.md](STABILIZATION_REPORT.md):

| Level | Meaning |
| --- | --- |
| EL0 | code inference |
| EL1 | unit or characterization test |
| EL2 | integration test |
| EL3 | fault injection |
| EL4 | real machine |
| EL5 | long run or adversarial |
| EL6 | formal proof |

Status words:

| Status | Meaning |
| --- | --- |
| **COVERED** | in contract, with evidence |
| **COVERED (weak)** | in contract, only EL0-EL1 evidence |
| **UNPROVEN** | meant to be in contract, no evidence |
| **OUT OF CONTRACT** | not tolerated: the system may lose data or stop |
| **KNOWN ISSUE** | evidence of a violation |

The baseline topology is:

- a replication group of one primary and one sync standby, on separate
  hosts;
- three or five witnesses, one per host;
- a client using `net::Client` sessions.

A group with no standby, or with an async one, has weaker guarantees,
listed under C7 and in CORE_CONTRACTS section 9.

## Core guarantees under test

These are the guarantees SNC is meant to give [PROPOSED as the v0.2
contract; CORE_CONTRACTS states each precisely]. The tables below say,
per fault, how far the code delivers them today.

| Code | Guarantee |
| --- | --- |
| G1 | Single serving owner per slot: never two groups serving one slot, never two primaries of one group serving |
| G2 | HA-durable acknowledgement survives: an OK given as HA_DURABLE is never lost by a covered fault |
| G3 | At-most-once application of a sequenced mutation, within the session horizon (as built; CORE_CONTRACTS G-3 proposes: an expired session refuses instead of running) |
| G4 | Obligations settle: every durable obligation (follow-up) is eventually applied exactly once, once the fault heals |
| G5 | Recovery restores the last LOCAL_DURABLE state and serves nothing before the recovery barrier |
| G6 | Liveness: service resumes after a covered fault without operator action |

## Process faults

| Class | Fault | Guarantees | Mechanism | Evidence | Status |
| --- | --- | --- | --- | --- | --- |
| P1 | Crash of one process (SIGKILL) | G1-G5; G6 for serving, but the fenced old primary needs `--rejoin`, so the group runs with no replica (LOCAL_DURABLE acks, unreported) until an operator acts | WAL group commit, recovery, witness promotion, term fencing, client resend | `test_crash`, `test_replication` (SIGKILL failover, 30K resends), `test_migrate` (67 crash points), `test_control.py`, Vultr P3 kill, P6 soak | **COVERED** (EL3-EL5) |
| P2 | Stall: the process is alive but unresponsive (SIGSTOP, CPU starvation, a long task) | G1, and G2 for writes acknowledged before the stall. After a false failover the group has no replica, so later OKs are LOCAL_DURABLE only (G2 at risk) and G6 needs an operator (`--rejoin`) | Witness suspicion after `failover_ms`; the old primary withdraws when it resumes and learns the new term | Vultr P3 `stop`; `test_health.py` T5 (SIGSTOP: promotion, old primary fenced after SIGCONT). Self-stall (STABILIZATION_REPORT D: a 3.7-5.0 s task on a shard thread made STATUS silent and the witnesses promoted): fixed for witnesses that poll `HEALTH` (HEALTH_DECOUPLING.md section 10; `test_health.py` T1, T3, T12). A shard wedged **forever** on a live process is now never failed over: accepted under option B, operator action, `stall_ms` shows it (HF1) | stop: **COVERED** (EL3-EL4); self-stall: **COVERED** (EL3) when every witness polls `HEALTH`, **KNOWN ISSUE** with any `STATUS` witness; wedge: **KNOWN LIMITATION** (option B) |
| P3 | Clean restart (SIGTERM, stop, start) | G2; G5 partial: follow-ups are held until the barrier, but node links and replication shipping start before it and the space-UUID check runs after it (CORE_CONTRACTS section 12, I-b, I-c, I-h) | Stop drains in-flight WAL (`quiesced`); no checkpoint is taken at stop (`engine.cpp:218-243`), so restart replays the WAL | `test_persistence`, `test_transfer` (clean stop with transfers in flight), `test_resend_refusals` (restart while sealed) | G2 **COVERED** (EL2); G5 **partial** |

## Host faults

| Class | Fault | Guarantees | Evidence | Status |
| --- | --- | --- | --- | --- |
| H1 | Host crash (all its processes at once) | As P1 for each process; witnesses are a majority elsewhere | Simulated by killing every process of a host on loopback (`test_migrate` crash matrix); Vultr P3 killed processes, not hosts | **UNPROVEN** at host level (EL3 by simulation) |
| H2 | Host stall (VM pause, steal) | As P2 | Not tested on a host. INFERRED: same as P2 for each process on it | **UNPROVEN** |
| H3 | Host power loss | G2 depends on the storage stack honoring `RWF_DSYNC`/`fdatasync` | None (EVIDENCE.md experiment L7) | **UNPROVEN** |

## Network faults

| Class | Fault | Guarantees | Evidence | Status |
| --- | --- | --- | --- | --- |
| N1 | Connection reset (RST) | G1-G6; the client resends with the same sequence | Vultr P3 `link` and kill; `test_net` (disconnect with 2,000 in flight) | **COVERED** (EL4) |
| N2 | Silent partition (no RST) | G1-G4; G6 after 7.7-8.5 s | Vultr P3 `partition` ×26, P4 partition 10 s | **COVERED** (EL4). The detection time is not explained (EVIDENCE section 5, EL0) |
| N3 | Asymmetric partition (one-way) | As N2 | Vultr P3 `oneway` ×1 | **COVERED (weak)** (EL4, one round) |
| N4 | Latency and jitter | G1-G4; G6 unless latency exceeds detection budgets | Vultr P4 `delay` 1/5/20 ms. No jitter test | **COVERED (weak)** for delay ≤ 20 ms; jitter **UNPROVEN** |
| N5 | Packet loss | TCP retransmits | None | **UNPROVEN** |
| N6 | Reordering | TCP orders each stream. Across streams, follow-ups and replication carry LSNs and sequences | None | **UNPROVEN** (EL0: no cross-stream ordering assumption found, not audited) |
| N7 | Flapping links | G1-G4. Repeated failovers are possible: no hysteresis | None. INFERRED: each flap longer than `failover_ms` can fail over; the fenced old primary needs `--rejoin` | **UNPROVEN** |

## Storage faults

| Class | Fault | Guarantees | Mechanism | Evidence | Status |
| --- | --- | --- | --- | --- | --- |
| S1 | Process death before a write is durable | The write was never acknowledged: lost, and the client resends | DURABLE acks wait for durability | `test_crash`, `test_replication` | **COVERED** (EL3) |
| S2 | Torn write at the WAL tail | Recovery stops reading a segment at the first bad record and treats the rest as a torn tail (`storage.cpp:396-414`) | CRC32C per record, torn-tail handling | `test_persistence` (torn tail, CRC) | **COVERED** (EL1-EL2) for a real torn tail |
| S3 | Write or fsync failure (EIO, ENOSPC) | Fail-stop: the process aborts (`storage_fatal`, citing the v0.1 storage spec's rule C7, fail-stop; not this document's C7); a failover follows if a standby exists | `src/core/storage.cpp` `flush_done`, `storage_fatal` | No test injects EIO or ENOSPC | **UNPROVEN** (EL0). A full disk in this work order stopped the lab, not SNC |
| S4 | Device loss | The standby holds HA_DURABLE data | Sync replication | As P1 for the node | **UNPROVEN** as a device fault (EL0) |
| S5 | Power loss | As H3 | | | **UNPROVEN** |
| S6 | Latent corruption (bit rot) | A bad record in the **middle** of the last WAL segment is indistinguishable from a torn tail: recovery silently drops it and every later, already-acknowledged record of that segment (counted only as `torn_tails`). In an earlier segment, the next segment's `first_lsn` no longer matches and the process aborts (`storage.cpp:383-385`). Checkpoint CRCs are checked on load. Live memory is not checked; no scrubbing | | `test_persistence` (torn tail, CRC on load) | **KNOWN GAP** (INFERRED, EL0): mid-log corruption in the last segment is **silent loss of acknowledged writes**; elsewhere fail-stop. A standby holds the lost records in sync mode |

## Control-plane and coordination faults

| Class | Fault | Guarantees | Evidence | Status |
| --- | --- | --- | --- | --- |
| C1 | Witness minority down | Decisions continue by majority (Paxos) | `test_witness.py` (one witness killed), Vultr witnesses on 3 hosts | **COVERED** (EL2-EL4) |
| C2 | Stale placement on a node | G1. A node serves a slot only under its adopted map; requests for others get E_MOVED with the epoch; node links show newer epochs (invariant 17) | `test_migrate` (no pushed map, 60 s polls), `test_resend_refusals` (restart with an older map: E_MIGRATING until refreshed) | **COVERED** (EL2-EL3) |
| C3 | Stale HA term (an old primary returns) | G1, G2. The standby refuses an older term; the old primary withdraws and answers E_FENCED; it rejoins only by restart (`--rejoin`, reseed). **Exception:** an async primary that is cut off keeps acknowledging LOCAL_DURABLE writes until it learns the new term (C7) | `test_replication`, `test_cluster` (live fence), Vultr P3 (every old primary fenced) | **COVERED** (EL3-EL4) |
| C4 | Split reachability (witnesses disagree) | G1: a DEGRADE grant and a promotion are never both chosen | `test_witness.py`; Vultr partition | **COVERED** (EL2-EL4); its share of the unexplained ~6 s of the 7.7-8.5 s detection (EVIDENCE.md section 5) is **UNPROVEN** |
| C5 | Control plane unavailable during restart | `resolve` falls back to the stored map and starts "provisional" (`control.h:169-175`); start fails only with no stored map or a used-up lease block (`engine.cpp:352-356`). After `--rejoin` the stored map is gone, so a rejoin with the control plane down fails | None | **UNPROVEN** (EL0) |
| C6 | Control plane unavailable during migration | The driver stops; the move state stays in the placement; every step resumes. The slot can stay sealed (unavailable) until the control plane returns | `test_migrate` (driver death at each step) | **COVERED** for the driver (EL3); witness-majority loss mid-move **UNPROVEN** |
| C7 | Stale replica promoted | Sync, not degraded: the standby holds every HA_DURABLE write. Degraded: promotion is refused while the grant is on record. **Async: the witness promotes a stale standby and acknowledged writes are lost** | `test_async_failover` (500/500 lost; sync control not promoted) | sync: **COVERED** (EL2-EL4); async: **KNOWN ISSUE** (R08), contract in CORE_CONTRACTS section 9; migration consequences: C7a |
| C7a | Stale replica promoted during or after a slot move (D1, D1b, D1c, D1d, D7 of docs/MIGRATION_FAIL_CLOSED.md) | MS-1 to MS-4 (CORE_CONTRACTS section 2): a slot whose arrival the promoted node cannot prove is not served (`proof=missing`); the barrier re-seals a slot being moved away before serving; the commit waits for the source group; an offline restart of a former standby's directory runs the same barrier and does not start without the control plane. Acknowledged writes lost by the promotion itself stay lost (C7, R08). A second source primary the driver cannot reach is not covered. Slots moved before placement v2 are covered only once backfilled | `test_fail_closed` F1-F11, F13, F15, F17, F18, role, backfill, stuck, prepared, f10_reseal, f7_prepared (compat and f7b need `--old`); F12 and the proof-cache cases in `test_proof`; F14 is the probes in the promotion scenarios; `test_placement`; characterization of the old behavior in bench/results/failclosed-20261104 | **COVERED** for reachable members (EL2, one machine); availability cost: an unproven slot stays unavailable (repair not built) |

## Workload-induced faults (added by this work order)

| Class | Fault | Status |
| --- | --- | --- |
| W1 | One unit of work monopolizes a shard thread (final stage, export, slot_report, checkpoint, rebuild_index) | The false failover it caused is gone with `HEALTH` (P2). Still a **KNOWN ISSUE** for the availability of the stalled shard: its clients wait for the whole task (final stage measured 4.8 s, T12); others **UNPROVEN** (EL0). CORE_CONTRACTS section 11, B-1 is the bounded-work rule (next slice) |
| W2 | Session expiry before a resend (retry horizon) | **KNOWN ISSUE** (R11: `test_session_ttl` resends an answered request after expiry and it applies again; a lost answer takes the same path, INFERRED); contract in CORE_CONTRACTS section 10 |
| W3 | Resource exhaustion: allocation failure in the session path aborts the process; the backlog refuses new work; growth is unbounded while durability stalls | **UNPROVEN** (EL0); inventory in STABILIZATION_REPORT J |

## Out of contract (current prototype)

- **Byzantine faults:** any node, witness or client that lies. There is
  no authentication on the admin, witness, node-link, replication or
  client ports (R05/R13).
- **Malicious clients**, for example forging another client's
  `client_id`, which would hijack its dedupe evidence.
- **Accidental `client_id` reuse or collision** (a restarted client that
  reuses its ID and restarts its numbering, or two random IDs that
  collide). Until sessions have server-issued identities
  (CORE_CONTRACTS section 10).
- **Production security.** The current isolation (separate ports, no
  authentication) is not a security boundary.
- **Synchronized clocks.** No guarantee may depend on them
  [ACCEPTED: work order]. Two dependencies exist today and are gaps, not
  exclusions: session TTL (W2, R11) and the standby read lease, which
  assumes bounded clock rate drift (CORE_CONTRACTS section 5). Paxos
  ballots read the wall clock for liveness only (CORE_CONTRACTS
  section 14).
  No safety property may depend on synchronized clocks
  (CORE_CONTRACTS).
- **Loss of both members of a group** (both hosts, both disks).
- **Loss of a witness majority:** no failover and no placement change
  until a majority returns. INFERRED: serving continues on current
  primaries, except that a sync primary that also loses its standby
  cannot get a DEGRADE grant, so its durable writes block.
- **Multi-tenant isolation** of any kind.
