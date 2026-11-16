# SNC-AFR-001: three core decisions, evaluated together (for owner review)

- **Status: PROPOSED.** This document decides nothing. A recommendation binds only when cody accepts it, after
  Codex's review. It authorizes no code, no C++ slice and no Rust work.
- **Scope.** Three decisions, evaluated together because each one constrains the others (Codex, PR #139 final
  review):
  - AFR-D-01, the authority model;
  - AFR-D-02, the durability boundary;
  - AFR-D-03, the replication-group model.
- **Baseline.** `main` at `15d3cf4`. Every file:line citation refers to that commit.
- **Inputs.**
  - `docs/SNC_GUARANTEES.md`, sections 14.1 and 14.2 (what the analysis must cover);
  - the Phase 1 notes `A_core.md` and `B_distributed.md`;
  - CORE_CONTRACTS section 8 (promotion eligibility, E1-E11);
  - the prior-art survey in section 7 (primary sources only).
- **Evidence labels.**
  - VERIFIED: read in code at the cited line.
  - TESTED: covered by a named test or experiment.
  - INFERRED: reasoned, not run.
  - UNMEASURED: a cost claim with no measurement yet.

## 1. Summary for cody

| Decision | Recommendation | Strongest evidence for | Strongest evidence against |
|---|---|---|---|
| **D-03 group model** | **An in-sync set under witness consensus.** The pair is widened to N members. Every member acknowledges every write. The witnesses own a versioned group record (term, set, mode, minimum set size), and a degrade (set shrink) and a promotion are compare-and-set decisions on it. Do not switch to majority-acknowledged replicas. | The C++ already does this for pairs: the safety pieces are TESTED for pairs under process kill. Published designs use the same shape: PacificA, Vertical Paxos II, chain replication, FoundationDB's log servers and Kafka's ISR. Two copies tolerate one failure; a majority quorum needs three. | The slowest member sets write latency (Kafka's own design notes concede this). Every member failure stalls writes until the witnesses shrink the set. A set shrunk to one copy is a single-copy window (section 3.2). |
| **D-01 authority** | **A term-scoped primary lease held from every member of the in-sync set**, the PacificA shape. Witnesses never decide a promotion or a degrade until every lease that could block it has expired. Fencing is durable. A restarted node re-acquires authority before serving. | It closes stale reads, O1 and non-durable fencing with one mechanism and keeps reads local (INV 14). PacificA, TiKV, CockroachDB and Spanner make leases the default. The C++ standby lease already has half the timing rule (F-5). | Ongaro's thesis does "not recommend" leases unless needed, and etcd defaults to quorum reads. A lease depends on a clock-rate bound, which is an assumption, not a checked fact (G-46). |
| **D-02 durability boundary** | **Log positions are (term, LSN), with a durable term-to-start-LSN history. Flushes and truncations are self-describing and term-stamped. The new primary computes the end of the log from its set, and a recovering member truncates to that point before serving.** A node without a replica gets detection only (G-14(a)), unless a watermark is chosen after measurement. | Kafka's KIP-101 is a postmortem of exactly this gap: high-watermark truncation lost committed data, and the fix was (leader epoch, offset). Raft (term, index), PostgreSQL timelines and pg_rewind, Aurora's epoch-stamped truncation ranges and FoundationDB's recovery version all follow the same pattern. | A locally checkpointed, never-acknowledged suffix cannot be truncated away, so the member must be reseeded (section 4). Single-copy windows weaken G-14(b) (section 3.2). |

**Owner dispositions so far:** D-01 lease chosen by cody (section 5.3). D-02 and D-03 pending.

The three decisions stand or fall together:
1. The in-sync set is both the lease grantor (D-01) and the holder of the acknowledgement boundary (D-02).
2. The term that fences authority is the term in every log position.
3. Restart is one barrier: reconcile the log, take the lease, then serve.

## 2. Facts that bind all three

| # | Fact | Evidence |
|---|---|---|
| F-1 | The HA unit is a pair (`Pair host[2]`). Placement allows N members, but HA state does not know them. | VERIFIED: `snc_witness.cpp:531-534`, `placement.h:44-48` |
| F-2 | A sync write is acknowledged only when it is locally durable and acknowledged by the standby. | VERIFIED: `shard.h:555-559` |
| F-3 | The degrade grant and the promotion are proposals on one consensus value, and they exclude each other. | VERIFIED: grant `snc_witness.cpp:423-513`, promotion `:852-872`; TESTED: `test_witness.py:240-256` |
| F-4 | Every promotion lands in degraded (NOT READY) until the new primary proves a standby in sync. | VERIFIED: `snc_witness.cpp:871` |
| F-5 | A primary may not degrade while a read lease it granted to its standby is live. The lease keeps a 10% margin for clock-rate drift. CORE_CONTRACTS records that drift safety as UNPROVEN. | VERIFIED: `shard.cpp:2254-2256`, `replication.cpp:743-745`; CORE_CONTRACTS:179 |
| F-6 | The primary holds no lease, and withdrawal is an in-memory flag. With witnesses, what remains is stale reads (QA 2 N6). Without witnesses, two primaries acknowledge writes. | VERIFIED: `engine.cpp:1968-1970`; TESTED: QA 2 N6, N7 |
| F-7 | Recovery cannot tell a zeroed or lost newest flush from unwritten space: segments and spares are preallocated with zeros, and a zero block ends the scan. | VERIFIED: `storage.cpp:393-395`, `:554-555`, `:776-787` |
| F-8 | After truncation the stream resumes at `last+1`, so LSNs are reused. Records ship before the local write (`storage.cpp:652`), so the standby may still hold the truncated records. | VERIFIED: `engine.cpp:1189`; INFERRED: the standby part |
| F-9 | Checkpoints wait for local durability only, so a checkpoint can contain records the standby never acknowledged. | VERIFIED: `shard.cpp:2710-2716` |
| F-10 | The superblock falls back silently to the older copy. A damaged copy is detected by its CRC; a lost write is not. The witnesses hold the lease block's bounds but not `next_lease` within it. | VERIFIED: `storage.cpp:210-226`, `engine.cpp:1065-1070` |
| F-11 | Membership is static: witness peers, HA pairs and placement are each configured separately. | VERIFIED: `snc_witness.cpp:556-569`, `:597-648` |
| F-12 | Every timer uses `steady_clock` (CLOCK_MONOTONIC), which stops while the machine is suspended. | VERIFIED: `src/native/runtime.cpp:38-41` |
| F-13 | Sync replication throughput on real hosts is 0.98-1.05x of non-replicated durable throughput; on loopback it is 0.54-0.60x. | TESTED: `REAL_MACHINES.md:237` |

## 3. AFR-D-03: the replication-group model

### 3.1 Comparison (spec section 14.2)

| Point | (A) In-sync set under external consensus | (B) Majority-acknowledged replicas (Raft-like) |
|---|---|---|
| Quorum intersection | Writes are acknowledged by every member of the current set. Promotion picks a member of that set. A set change and a promotion are compare-and-set on one versioned record (F-3), so every promotable member holds every acknowledged write. **Rule for N ≥ 3:** the primary acknowledges against a smaller set only after the decision is chosen. | Any two majorities intersect, and the elected leader holds every committed entry. |
| Data commit | Committed when every set member has it durably. A member may hold an unacknowledged suffix (F-8), which is in-doubt (G-58). | Committed on a majority. Minority suffixes are truncated by the leader. |
| Witnesses | Decide term, set, mode and placement. They hold no data. Changing the witnesses themselves is AFR-D-13 (static today, F-11). | Not needed for data. Placement still needs a consensus service. |
| Promotion eligibility | E1-E11 (CORE_CONTRACTS 8), with E1 resolved: a member outside the set, async included, is never promoted automatically (closes R-10). E2 and E5 become "every member of the set proved in sync". | The most up-to-date log wins the election. |
| Membership change | One compare-and-set on the group record per change. A joining member is a non-voting candidate until it has caught up (as PacificA candidates and Vertical Paxos II activation do; the C++ readiness proof already works this way). | Single-server or joint-consensus changes, with known subtleties (Ongaro, chapter 4). |
| Failures tolerated | f with f+1 copies, but only while the witnesses are reachable to shrink the set. After a shrink, one copy remains (3.2). | f with 2f+1 copies, with no external dependency. |
| Write latency | Set by the slowest member. On real hosts, a pair costs almost nothing (F-13). A slow member stalls writes until it is removed (F6, F7). | Set by the median member, so slow replicas are hidden. |
| Recovery work | The new primary computes the end of the log from the set (section 4). A rejoining member truncates or is reseeded. | The same, from a majority. |
| Code and test surface | The existing pair machinery, widened. The C++ stays a valid behavioral oracle. | A new replication and election protocol, which the C++ cannot judge. |
| Evidence | Pairs under process kill: EL2-EL4. N ≥ 3: none. Host crash: UNPROVEN (G-13). | None in SNC. |

### 3.2 Single-copy windows (a weakening that must be stated)

**When.** A degraded group, and every group immediately after a promotion until it is READY again (F-4), runs on one
copy.

**Consequence.** During those windows G-14(b) holds only as well as one disk does. This is the weakness of the
Kafka `min.insync.replicas=1` case. Kafka's KIP-101 records that divergence is still not prevented there, and
Redis Sentinel and PostgreSQL show the operational pressure to accept single-copy writes.

**Proposed.** A configurable **minimum set size**, recorded in the group record:
- **Minimum 2:** refuse writes while one copy remains.
- **Minimum 1 (today's behavior):** accept them, but report the achieved class as LOCAL_DURABLE (G-12).

The owner chooses the default.

### 3.3 Recommendation: (A)

**Why (A).** Its safety rests on the exclusion F-3, which is tested. It matches the published primary-backup family
(PacificA, Vertical Paxos II, chain replication, FoundationDB logs, Kafka ISR). It needs two copies where (B) needs
three, and on real hosts its latency cost is small (F-13).

**What (A) must add to the C++:**
- one versioned group record derived from placement (fixes F-1 and F-11), holding the term, set, mode and minimum
  set size;
- consensus-driven membership changes, with candidates that catch up before they join;
- a guarantee that async members are never in the set.

**Revisit (B) if** experiment X-1 shows the slowest-member tail latency breaks the validation campaign's latency
criteria.

## 4. AFR-D-02: the durability boundary

| Case | Detectable locally? | Remedy |
|---|---|---|
| Damage inside a flush; unknown record kind | Yes | Fail closed (AFR-S-1) |
| Newest flush zeroed | Only if unwritten space differs from zeros | Format segments with a per-segment nonce that every flush carries (extra write per segment, UNMEASURED, X-2) |
| Newest flush silently lost (acknowledged by the device, never written) | No | A second durable copy: the set (replicated) or a per-flush watermark (single node, X-2) |
| Superblock newest copy damaged | Yes (CRC) | Refuse, or fall back **and** take a fresh mint lease block, then check term and epoch against the witnesses |
| Superblock newest write lost | No | The same: check against the witnesses before serving, and always take a fresh lease block, since `next_lease` is not held by the witnesses (F-10) |
| LSN reuse (F-8) | — | (term, LSN) positions |

### Recommendation

1. **Positions are (term, LSN).** The term is the group-wide HA term, not a node-local incarnation, so positions
   compare across members. Each member keeps a durable term-to-start-LSN history, as Kafka's leader-epoch file
   does. A single node that discards a torn tail resumes after the highest LSN named in any flush frame, so it
   never reuses a position.
2. **The end of the log is decided by the new authority, never by a member alone.**
   - The new primary's log is the reference for its term.
   - Records it holds that the old set never fully acknowledged become committed once the new set acknowledges
     them (PacificA re-commit). Records it lacks are discarded.
   - No client ever received an OK for a discarded record (G-58: such writes are in-doubt).
   - A rejoining member asks the primary for the end offset of its own last term (as Kafka's OffsetsForLeaderEpoch
     does) and truncates to it.
3. **Truncation must not undo a checkpoint.** If the divergence point is below the member's checkpoint maximum LSN
   (F-9), the member is reseeded instead of truncated. The alternative is to bound checkpoints by the set's
   acknowledged point, which reopens the B1 wedge recorded in `shard.cpp:2710-2716`, so it is not recommended.
4. **Flushes and truncations are self-describing and term-stamped.** Each carries its range, the term, the segment
   nonce and a checksum. Truncations are written durably, so an interrupted recovery restarts safely (Aurora's
   epoch-versioned truncation ranges). This is a format change and waits for AFR-D-06.
5. **A node without a replica gets G-14(a) only.** The owner may instead choose a per-flush watermark, after X-2
   measures its cost.

The minimal fail-closed C++ slice (AFR-S-1) needs none of the format changes above.

## 5. AFR-D-01: the authority model

### 5.1 The lease

- **Validity.** A primary may answer a default read or acknowledge a write only while it holds a valid lease for its
  term from **every** member of the current in-sync set. Its lease is the minimum over those grants (the PacificA
  shape).
- **Renewal.** Leases are renewed on acknowledgement traffic, with a heartbeat when the primary is idle.
- **Who waits.** A member that is promoted was itself a grantor, so it waits until its own last grant has expired.
  That wait suffices for any N, because the lease needed grants from every member.
- **Degrade.** A degrade waits until every read lease the primary granted to the leaving member has expired. The
  C++ already does this (F-5).
- **Degraded mode.** Here the set is the primary alone and needs no lease. Automatic promotion is impossible while
  degraded (F-3). Operator promotion carries the expected term (PE-2) and waits one full lease length.
- **Grantor restart.** After a restart, a grantor (or a member being promoted) waits one full lease length before it
  grants or serves. This removes any need to persist grants.
- **Clocks.**
  - Lease time is measured on a clock that keeps counting through suspend (CLOCK_BOOTTIME, not today's
    CLOCK_MONOTONIC, F-12).
  - The holder checks validity when it produces the answer, not when it receives the request.
  - The holder's lease is shortened by a stated rate-drift bound. The lease is shorter than the witnesses' failover
    timeout (TiKV uses 9 s against 10 s).
  - Exceeding the drift bound is outside the contract (G-46), not something the node can detect. A virtual-machine
    pause that hides time from the guest is also outside the contract.
  - Optionally, a node fences itself when it observes a large offset against its peers (CockroachDB's 80% rule).
- **Stale-authority rejection, on every path.** A node that learns of a newer term, or whose lease has expired:
  1. persists the newer term before answering anything else;
  2. refuses with a status that names the newer term, or says that its authority is unproven;
  3. reports itself as not primary (G-34).

### 5.2 Scenarios (spec section 14.1)

| Scenario | Today | With (A) + lease |
|---|---|---|
| Partition: primary cut from its standby and the witnesses | No writes are acknowledged, but stale reads continue indefinitely (F-6) | The lease expires and the primary refuses. The witnesses promote only after the failover timeout, which is longer than the lease, so the two never overlap (G-28, G-31) |
| Stall and resume (SIGSTOP, then SIGCONT, after a promotion) | Reports `role=p` until it hears of the new term (O1). Reads are possible until then | On resume, the validity check at answer time finds the lease expired. The node refuses and learns the new term durably (G-34) |
| Restart while fenced | Serves at the old term (F-6) | The persisted highest term refuses the old term. Reconcile, take a lease, then serve (G-32) |
| Promotion during migration | Seal (MS-1 to MS-4), plus a commit gate that sees only reachable members | Unchanged rule: seal **and** expiry of any older-term source lease, both required. Leases do not remove the need for the seal |
| Set change (degrade, rejoin, add) | Pair only; the degrade waits on the standby lease (F-5) | Each change waits on the leases it could break, then a compare-and-set on the group record |
| Machine suspend | Monotonic clock stops, so a lease would fail open | CLOCK_BOOTTIME counts through suspend (VM pause stays out of contract) |

### 5.3 Recommendation: the lease, as specified in 5.1

**Why.** It keeps every default read local (INV 14). It closes the stale-read, O1 and fencing gaps together. It
reuses the shape of the existing standby lease (F-5).

**The main point for the owner to challenge.** Raft's author recommends quorum reads unless performance requires
leases, and etcd's default follows that advice. If cody prefers no clock assumption in the default, the alternative
is quorum-confirmed reads (a round trip to the set per read batch). That alternative needs no grace rules, but it
pays the round trip that SNC's local-execution thesis is meant to avoid. Offering both modes is deferred, because it
doubles the authority surface and the oracle modes.

**Owner disposition (cody, 2026-11-16 01:25 UTC), verbatim:** "Okay, so okay. Question 2, you said you need me.
Authority. Yeah, let's go with the lease." This chooses the lease over a per-read round trip as the default. It is
recorded ahead of Codex's review of this document, so the grace rules in 5.1 stay open to Codex's challenge. It
authorizes no implementation. D-02 and D-03 have no owner disposition yet.

**Harness evidence.** For the section 15 oracle, each answer's authority evidence (term, plus the lease or grant it
relied on) is recorded in the test harness's history. This does not change the client wire format.

## 6. Couplings and measurements

The couplings:
- Under (A), the set grants the leases and holds the boundary.
- Restart is a single barrier: reconcile, then lease, then serve.
- The term fences authority and positions log records.
- The witnesses hold the term and the lease-block bounds that the superblock check needs.
- Async members are outside the set, so they never grant leases and are never promoted automatically.

Choosing (B) would change every one of these couplings. Accepting the three decisions separately is therefore not
recommended.

| Experiment | Measures | Decides |
|---|---|---|
| X-1 | p50 and p99 write latency with a set of 2 and of 3, each with one slow member (R640) | (A) or (B) |
| X-2 | The cost of a per-flush watermark, and of nonce-formatting each segment (R640 data disk) | D-02 items 1 and 5 |
| X-3 | Lease renewal and heartbeat overhead, and the failover delay the lease adds | Lease and timeout lengths |

Each experiment needs its own preregistration and go (AGENTS.md change discipline). None is started.

## 7. Prior art (primary sources)

Every row was checked against the source linked. Where a source does not cover something, that is stated.

| System | Authority and reads | Committed point and recovery | Group model | Source |
|---|---|---|---|---|
| Raft and Ongaro's thesis | Leader per term. ReadIndex reads cost a quorum round. A lease alternative needs a drift bound and is "not recommend[ed] unless necessary" | Majority. (term, index) log matching; the leader overwrites conflicting entries | Majority, membership inside the group | [paper](https://www.cs.princeton.edu/courses/archive/spring22/cos418/papers/raft.pdf), [thesis 6.4](https://raw.githubusercontent.com/ongardie/dissertation/master/online.pdf) |
| etcd | Linearizable (quorum) reads by default. Lease reads are opt-in, "affected by clock drift", and need CheckQuorum | Raft | Raft; warns against 2-member clusters | [guarantees](https://etcd.io/docs/next/learning/api_guarantees), [raft](https://pkg.go.dev/go.etcd.io/raft/v3) |
| CockroachDB | Leaseholder reads locally. Moved from expiration leases to epoch leases (whose liveness dependency caused unavailable ranges) to leader leases tied to Raft leadership. A node shuts itself down at 80% of the maximum clock offset | Raft | Raft per range | [replication](https://docs.cockroachlabs.com/docs/stable/architecture/replication-layer), [leader leases](https://cockroachlabs.com/blog/distributed-database-leader-leases) |
| TiKV | Lease 9 s against a 10 s election timeout, started at proposal time, measured on a raw monotonic clock | Raft | Raft | [lease read](https://tikv.org/blog/lease-read/) |
| Spanner | Disjoint leader leases enforced by TrueTime (ε about 1-7 ms) | Paxos majority | Paxos groups | [OSDI 2012](https://courses.cs.duke.edu/spring15/cps210/readings/spanner-osdi2012.pdf) |
| FoundationDB | Coordinators elect a controller, and one sequencer per epoch locks the coordinated state | Commit means **all** log servers persisted it. Recovery takes RV = min DV of the old logs and discards everything after it | Any log failure triggers a reconfiguration | [SIGMOD 2021](https://www.foundationdb.org/files/fdb-paper.pdf) |
| Kafka | The controller elects the leader from the ISR (no read lease found in the pages checked) | Committed = all ISR members, with a floor of `min.insync.replicas`. KIP-101: truncation by high watermark lost data; fixed with a (leader epoch, offset) history | ISR, kept by the controller | [design](https://kafka.apache.org/43/design/design/), [KIP-101](https://cwiki.apache.org/confluence/spaces/KAFKA/pages/67634337/KIP-101+-+Alter+Replication+Protocol+to+use+Leader+Epoch+rather+than+High+Watermark+for+Truncation) |
| Aurora | Single writer; reads go to a segment that is complete at the read point | 4/6 write quorum. VCL and VDL; recovery recomputes VDL from a read quorum and writes durable, epoch-versioned truncation ranges | Quorum per protection group | [SIGMOD 2017](https://pdos.csail.mit.edu/6.824/papers/aurora.pdf) |
| PostgreSQL | No built-in failover. The docs require STONITH against two primaries | `synchronous_commit` levels; `FIRST n` or `ANY n` standbys. pg_rewind uses timelines to find the fork point | External, operator-managed | [failover](https://www.postgresql.org/docs/current/warm-standby-failover.html), [pg_rewind](https://www.postgresql.org/docs/current/app-pgrewind.html) |
| **PacificA** | The primary holds a lease from **each** secondary. A secondary may propose itself as primary only after a grace period that is at least the lease | Committed only when **all** replicas acknowledge. A new primary re-commits its prepared entries, then has the others truncate | External Paxos configuration manager with versioned configurations, first accepted wins. New members join as candidates | [MSR TR 2008-25](https://www.microsoft.com/en-us/research/wp-content/uploads/2008/02/tr-2008-25.pdf) |
| Vertical Paxos | "A new primary cannot be chosen until that lease expires" | VP II: the write quorum is all members | External master. A new configuration is activated only after state transfer completes | [PODC 2009](https://microsoft.com/en-us/research/uploads/prod/2009/08/Vertical-Paxos-and-Primary-Backup-Replication-.pdf) |
| Chain replication and CRAQ | Reads at the tail (CRAQ: any node, asking the tail for dirty objects). Fail-stop servers are assumed | Complete when the tail has processed the update (every node has it) | Paxos master; t servers tolerate t-1 failures | [OSDI 2004](https://pdos.csail.mit.edu/6.824/papers/cr-osdi04.pdf), [CRAQ](https://www.cs.princeton.edu/~mfreed/docs/craq-usenix09.pdf) |
| Redis Sentinel (cautionary) | Configuration epochs. A partitioned old master keeps accepting writes, which are "lost forever" | Asynchronous replication | External Sentinel majority | [docs](https://redis.io/docs/latest/operate/oss_and_stack/management/sentinel/) |

**What SNC takes from it.**
- SNC's pair plus witnesses already belongs to the PacificA and Vertical Paxos family. It has their versioned
  configuration, but **not their timing rule**: no new primary until the old lease has expired. That missing rule
  is exactly the stale-read gap. D-01 adds it in the form those papers use.
- Every surveyed system lets the new authority compute the end of the log, and stamps log positions with a term or
  epoch. D-02 follows that pattern. Kafka's KIP-101 is the cautionary case of doing it without epochs.
- The all-acknowledge family pays with latency from its slowest member and with stalls on member failure. Majority
  systems hide both. X-1 measures whether that matters for SNC.
- Leases as the default are common (PacificA, TiKV, CockroachDB, Spanner) but not universal (Raft guidance, etcd).
  Section 5.3 leaves this as the explicit choice for cody.
- Redis Sentinel and the single-copy cases show why the minimum set size (3.2) must be an explicit, reported
  setting.

**Not verified.**
- How CockroachDB's lease validity relates to its maximum clock offset.
- Read fencing of an old Kafka leader.
- Aurora's membership-change protocol.
- PacificA's treatment of non-zero clock drift: its proof assumes zero drift.

## 8. What acceptance would freeze

**Frozen:**
- the group model: an in-sync set under a versioned group record;
- authority as a term-scoped lease from every set member, together with the grace rules in 5.1;
- the restart barrier;
- (term, LSN) log identity, with the new primary deciding the end of the log.

**Not frozen:**
- byte formats (AFR-D-06);
- the trust model (AFR-D-07);
- lease and timeout lengths (X-3);
- the watermark (X-2);
- the default minimum set size (3.2);
- witness membership change (AFR-D-13).

Acceptance would not prove that the C++ meets any of this, and it would not authorize Rust (spec section 16).
