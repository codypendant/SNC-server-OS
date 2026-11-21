# AFR-001: build versus reuse for replication, consensus and durability (PROPOSED)

Status: **RESEARCH AND DECISION EVIDENCE ONLY** (Codex, 2026-11-21, `docs/handoff/PR181_PR182_Codex_Review.md`): the HYBRID recommendation is **not accepted**; raft-rs/OpenRaft adoption and RX-1 are on **HOLD**, and the top-level product decision is reopened first. Originally: **PROPOSED decision analysis, for Codex and the owner.** Asked for by Codex's review of PR #180, sections 5-7,
relayed by cody in the project thread on 2026-11-19 20:57 UTC; that review is not yet filed in `docs/handoff/`, so
its section numbers below refer to the relayed text. Docs only: no code, no experiment, no model run, no R640. No frozen
or accepted text changes here; every change this document recommends is listed in section 7 as a decision for cody.
Sections 1-4 of that review (the `Combined.tla` fix, the L-1 runs, LiveGrant) belong to the other AFR thread and are
not touched.

Labels (docs/ENGINEERING_PRINCIPLES.md):
- **FACT** read in this repository (file named).
- **EXT** an external system's documented behaviour or source, with the URL. **EXT-SRC** was read in a source clone
  (OpenRaft `main` at 8de65a99, raft-rs `master`, TiKV `master` Cargo.toml). **EXT-WF** came through a fetch helper
  that summarises pages, so its quotes are not byte-verified (KIPs, PostgreSQL, Patroni, FoundationDB, Redpanda,
  papers). Section 10 lists what could not be verified.
- **INF** inferred here. **HYP** an untested guess.

## 1. Recommendation

**HYBRID.** Replace SNC's custom authority, membership and replication protocol with **Raft, using TiKV's `raft-rs`
as the consensus core**, one Raft group per shard stream of a replication group, **three full voting replicas, commit
quorum two.** SNC keeps everything above and below that core: the state-native object runtime, per-core execution,
its own WAL as the Raft log storage, checkpoints as Raft snapshots, the transport, placement, and the single-node
durability contract (fault model, fail-closed recovery, D-02).

What this retires (if cody accepts it): the witness-coordinated in-sync set (D-03 direction (a)), R2′, R7, R9-A..E,
E1-E11 / P1-P7 / S1-S5, R6.2/R6.3′ reconciliation, the term-scoped grant leases of D-01, LG-1..LG-6, A-1, and the
witness Paxos as the HA decider. These are where every counterexample since round 5 came from (FACT:
docs/AFR_001_LIVE_GRANT_PROPOSAL.md section 1; L-1 and LG-F1..F4 in PR #180).

What stays: the witness Paxos service itself, because it also holds the placement map, slot moves and mint lease
blocks (docs/REPLICATION.md, docs/CLUSTER.md). Under the recommendation SNC runs two consensus systems, per-shard Raft
for data and the witnesses for placement, kept apart exactly as invariant 12 already keeps placement epoch and HA
term apart. Moving placement onto a Raft group too is possible later and is not proposed here.

What it does **not** retire: G-14 and D-02 (a Raft voter that silently loses its persisted tail breaks quorum
intersection, so local detection is still required), R-23 (a rolled-back vote is a double vote), O_DIRECT and the
FM-1..16 fault model, and the cross-shard consistent cut.

**The deciding reason is protocol risk** [INF from FACT]. SNC's design belongs to a known family (f+1 data replicas
plus an external configuration decider, section 3.2) whose safety lives in the interaction between decider and
replicas. Kafka spent about eight years of KIPs closing data-loss cases in that layer, and AFR-001 keeps finding the
same cases (L-1 is KIP-497's shrink, LG-F3 is KIP-903). Raft folds the decider into the replicated log; its safety
argument is published and short, and etcd-lineage implementations have run in production for a decade.

**The second reason is that the custom protocol's cost advantage is smaller than it looks.** With `m = 2`, a
two-data-copy group cannot accept a write after any single data-node failure (G-12 [ACCEPTED]; the `m = 2` decision is PR148
section 3). The owner chose durability over availability on purpose and asked that the "failover availability
consequences must be revisited after the R2 comparison. Do not silently weaken the default to improve availability"
(section 3). Nothing here weakens it: the argument is about which protocol delivers `m = 2` with write availability,
not about lowering `m`. Keeping writes through one failure *and* two durable copies needs three data copies under any
protocol, custom included (section 3.1). At three copies, Raft with quorum two gives SNC's accepted guarantee by
construction. A cheaper variant (two data voters plus one log-only voter, about 1.0x RAM) exists but needs SNC-built
retention and catch-up rules; it is the first follow-up, not the starting point (section 3.1).

**Why not BUILD:** the one requirement no existing implementation meets is "HA with two full data copies plus
data-less witnesses" (section 3.1). That is a cost requirement, not a correctness one, and it is not in any accepted
guarantee. At equal cost the fair comparison is BUILD with three data copies (A ⊇ S and R2′'s verified survivors keep
writes through one loss) against Raft with three: then BUILD's only difference is the protocol it still has to prove
and build (4.1). Neither side's integration effort has been estimated; the claim is about the kind of risk, a
published protocol with a documented integration surface against an open-ended one, not about size.

**Why not ADOPT (PostgreSQL+Patroni or FoundationDB as the store):** both move authoritative state out of the owning
core and put a remote transaction system on every mutation, which removes SNC's thesis (sections 4.4, 4.5).

**Why raft-rs over OpenRaft:** raft-rs is sans-IO (the shard thread calls it; no runtime, no tasks) and descends
from etcd raft. OpenRaft schedules its own cooperative tasks; with its thread-per-core runtimes and `single-threaded`
feature they can run on the shard's core, so the gap is smaller than "owns an event loop" suggests, but it is pre-1.0
with recent safety fixes (sections 3.3, 4.3). The choice is close: OpenRaft is the named fallback, and RX-1 checks
the shard-loop fit of both before committing.

**What it costs, plainly:** one more full replica per group (about 1.5x cluster RAM, disk and apply CPU for the same
data, and twice the leader's replication egress), a Rust dependency we do not control, and a revision of owner
decisions D-01, D-03, R2′ and R9, of invariants 6 and 14, and of the standing prior-art rule (section 7). The witness
service stays for placement, so its three witnesses are not saved. It needs cody's explicit yes, and
then one bounded experiment (section 8) before anything is committed to.

## 2. What SNC needs from this subsystem

From the accepted and proposed texts [FACT: docs/SNC_GUARANTEES.md, docs/AFR_001_DURABILITY_CONTRACT.md,
docs/handoff/PR148_Owner_Decisions.md, AGENTS.md invariants]:

- **G-11/G-12** [ACCEPTED parts]: HA_DURABLE means at least two durable copies; new writes are admitted only while at
  least `m` (default 2) copies can be made; `m = 1` only per group, and then LOCAL_DURABLE.
- **G-14, G-15**: no silent loss (fail closed on detected damage); one `(term, LSN)` never names two contents.
- **G-31/G-33**: one serving owner; bounded authority.
- **G-35, G-48**: operator promotion that can lose data needs an explicit acknowledgement; covered faults heal
  without an operator.
- Invariants 3, 4, 11-14: one owner per object, NUMA-local execution, failover is not migration, placement epoch
  separate from HA term, the hot path does no control-plane consensus.
- Section 7 of Codex's review (relayed text): durable tasks, events, timers, agent state and state-native scheduling all live as
  objects in shards, so they inherit whatever this layer provides.

**An observation that makes Raft fit** [FACT: docs/REPLICATION.md "Model", docs/CORE_CONTRACTS.md section 13]: SNC
already replicates **per shard stream**, primary stream *i* to standby shard *i*, shipping **full post-op images**
that the standby installs without re-executing. Cross-shard work is already explicit **obligations** recorded in the
source shard's WAL group, not distributed transactions. So each shard stream is already a replicated log of
deterministic state transitions, which is exactly a Raft replicated state machine. Nothing needs a log shared across
shards.

## 3. The decisive structural questions

### 3.1 `m = 2`, witnesses and quorum size

| Layout (per group) | Durable copies at ack | Writes after one data-node loss | Automatic failover | Protocol |
|---|---|---|---|---|
| Today: 2 data + 3 witnesses, `m = 2` | 2 | **refused or wait** until a new replica catches up (G-12) [FACT] | yes, data and reads | custom (AFR-001) |
| Custom, 3 data + 3 witnesses, `m = 2` (A ⊇ S allows it) | 3, or 2 after a shrink | yes | yes | custom (AFR-001), with 3 copies and still the custom protocol |
| 2 data + 3 witnesses, `m = 1` opt-in | 1 after degrade (LOCAL_DURABLE) | yes | yes | custom |
| Raft, 3 full voters, quorum 2 | ≥ 2 (each voter persists before acknowledging, 4.2) [EXT-SRC] | **yes**, still 2 copies | yes | Raft |
| Raft, 2 full voters + 1 witness voter that compacts like TiKV's | **1 data copy + 1 log copy** when the quorum is {leader, witness}, and the log copy can be compacted | yes | yes | Raft plus custom witness handling |
| Raft, 2 full voters + 1 log-only voter that keeps its log until both data replicas have checkpointed past it [HYP] | ≥ 2 durable log copies | yes; the surviving data replica catches up from the log voter, then takes over | yes | Raft plus SNC-built retention, catch-up and "never leader" rules |
| Raft, 2 voters | 2 | no; no election possible either | no | Raft |

The witness-voter row is the trap Codex warned about. A witness that holds only the log tail is a durable copy of
that entry **only until it compacts it**, and it cannot be promoted to serve. G-11's [PROPOSED] text counts only
"automatically promotable" copies, so that layout would need G-11 revised, plus custom rules for when the witness
may truncate and how a data replica recovers from it. TiKV built exactly this in its raftstore, not in raft-rs, gives
the witness a negative election priority, and removed its witness schedulers in v8.0.0 as "not GA"
[EXT-SRC: TiKV `peer.rs` `priority: if peer.is_witness { -1 } else { 0 }`; EXT-WF:
https://github.com/pingcap/docs/blob/master/releases/release-8.0.0.md]. OpenRaft has no witness role; its nearest
role, a voter that never campaigns, still keeps the full log and state machine [EXT-SRC: OpenRaft
`membership/src/docs.md`]. **So this document does not assume a witness substitutes for a replica, and recommends
three full voters to start.** The log-retaining voter (last row) is a real option, raised by the independent review:
compaction is a retention policy, not a property of witnesses. It would bring RAM back to about 1.0x, which matters
most in an in-memory system. Its costs: G-11's "automatically promotable" wording changes to "durable log copies", a
catch-up window after failover in which only one full state copy exists, a log that can grow until both data replicas
checkpoint, and SNC-owned rules (it never leads, it truncates only behind both checkpoints). That is a small,
local rule set on top of Raft rather than a new protocol, but it is still ours. Proposed as the first follow-up after
RX-1, with its own G-11 revision.

**`m` under Raft** [INF]: a 3-voter group with quorum 2 never acknowledges with fewer than two durable copies, so
`m = 2` holds by construction; admission is "a quorum is reachable". `m = 1` becomes a one-voter group, whose answers
are LOCAL_DURABLE, which is the accepted rule. G-12's "the status surface identifies degraded redundancy" stays: a
group with 2 of 3 voters live still writes at HA_DURABLE but has reduced protection for earlier writes, and must say
so.

### 3.2 Prior art says SNC's current design is a known family, and a hard one

SNC's model, f+1 data replicas that all acknowledge plus an external configuration decider, is the
**Vertical Paxos / Cheap Paxos / PacificA** family [EXT-WF]: Vertical Paxos "use[s] an auxiliary configuration master"
and lets "a state-machine implementation ... tolerate k failures using only k + 1 processors" (Lamport, Malkhi, Zhou,
MSR-TR-2009-63, https://www.microsoft.com/en-us/research/publication/vertical-paxos-and-primary-backup-replication/);
Cheap Paxos "tolerates up to f failures with f+1 main processors and f auxiliary ones" (Lamport and Massa, DSN 2004,
https://www.microsoft.com/en-us/research/publication/cheap-paxos/); PacificA (Lin et al., MSR-TR-2008-25). Kafka's
ISR with a controller and FoundationDB's write-all log servers with recovery generations are the production members.

The production record of that family is the evidence [EXT-WF, cwiki.apache.org KIP pages]: Kafka fixed acknowledged-
data loss or divergence in this layer with KIP-101 (truncation by high watermark could lose data "after a fast leader
change"), KIP-279 (divergence after fast failover), KIP-497 (ISR changes centralised at the controller with
compare-and-set), KIP-903 (a replica with a replaced disk re-added to the ISR by a delayed request), and KIP-966
(eligible leader replicas, unclean shutdown). That is the same sequence of bugs AFR-001 has been finding (L-1 is
KIP-497's shrink case, LG-F3 is KIP-903). FoundationDB made its version work, and says "It seems unlikely that we would
have been able to build FoundationDB without" deterministic simulation (https://apple.github.io/foundationdb/
testing.html). Redpanda, building a Kafka-compatible system from scratch, chose "a Raft group" per partition instead
(https://docs.redpanda.com/24.3/get-started/architecture).

[INF] The lesson is not that f+1 designs are wrong; it is that their safety lives in the interaction between the
decider and the replicas, which is open-ended and cannot be closed by text review. Raft folds the decider into the
replicated log itself, which is why its proof is short and why its implementations converge.

### 3.3 Per-core shards, NUMA and multi-group cost

- **Mapping** [INF]: one Raft group per (replication group, shard index), each driven by that shard's pinned thread on
  every replica. Log, apply and snapshot stay on the owning core and its NUMA node, as today. A node with 36 shards in
  one replication group runs 36 groups.
- **raft-rs fits this directly** [EXT-SRC]: its README says it "includes the core Consensus Module only ... you will
  need to build your own Log, State Machine and Transport components"; the caller drives `RawNode` with `tick`,
  `step`, `ready` and `advance`. No thread, runtime or timer is imposed, so the shard loop owns it. TiKV runs many
  groups per store through a batch system (`store-max-batch-size`, `hibernate-regions` default on since v5.0.2)
  [EXT-WF: pingcap/docs `tune-tikv-thread-performance.md`, `tikv-configuration-file.md`].
- **OpenRaft fits less well** [EXT-SRC]: "The RaftCore thread is spawned when a Raft node is created", plus a state
  machine task and one replication task per target; it describes itself as "an asynchronous, event-driven Raft
  implementation". Its `AsyncRuntime` trait (since 0.9.0) and `single-threaded` feature could put one runtime per
  core, but the library, not the shard, schedules the work. Its multi-raft crate shares connections, and its example
  notes "Every group sends its own heartbeats".
- **Cost** [INF, HYP for numbers]: per group, one heartbeat per follower per tick and a small amount of state. Batched
  per destination node, 36 groups cost far less than one replication stream's data. Must be measured (section 8).
- **Hidden multi-group costs, all SNC-built** [INF, from the independent review]: when a node dies, 36 independent
  elections can scatter a replication group's leaders over the survivors, so cross-shard obligations start crossing
  nodes until leaders are transferred back (priority plus leader transfer); raft-rs's `Snapshot` is one message, so a
  large in-memory checkpoint needs out-of-band chunked transfer (today's seed path; TiKV uses snapshot files); log
  retention is tied to checkpoint cadence, so a follower that lags past it needs a full snapshot; hibernation of idle
  groups is a TiKV raftstore feature, not raft-rs; a disk replacement is one configuration change per shard group.
- **Execute before commit** [INF, from the independent review]: SNC's primary runs the operation and then logs the
  post-op image, so the leader's in-memory objects run ahead of the committed log. Clients are safe because
  acknowledgements and reads wait for durability (today) and would wait for commit (Raft). But a deposed leader whose
  uncommitted entries are truncated holds objects in a state no log contains. Today's demoted primary has the same
  problem and rejoins by reseed; under Raft the simplest rule is the same: a shard whose uncommitted entries are
  truncated rebuilds from its checkpoint plus the committed log. Later uncommitted operations that read that state are
  later in the log and are truncated with it. This must be designed, and it is RX-1's first test.
- **Apply before persist** [EXT, from the independent review, not re-checked here]: raft-rs master added
  `max_apply_unpersisted_log_limit` (PR #537, 2024) so a node may apply entries before persisting them, which TiKV uses.
  SNC must keep it at 0, so a checkpoint used as a snapshot never covers entries the node has not persisted, and the
  client acknowledgement waits for commit at the leader, never for a follower's apply.
- **Invariant 14** ("the hot execution path ... does not perform consensus"): Raft commit is in the write path. Today's
  sync standby acknowledgement is already a round trip to a replica in that path, so the latency shape is unchanged;
  the invariant was written about control-plane state (placement). It needs that clarification recorded (section 7).

### 3.4 Kafka's question: reject at the decider, or refuse at the node (LG-F3)?

**Under BUILD: reject at the decider, and keep the node's refusal as a backstop. Under Raft the roles invert,
because Raft has no separate decider** (see the end of this section). [EXT-WF] KIP-903's controller "make[s] sure all
the broker epochs in the ISR match with the latest broker epochs before persisting this change" and returns
`INELIGIBLE_REPLICA`; the follower only reports its epoch. KIP-853 gives each KRaft voter an identity of replica id
**plus a directory UUID**, because "different values means that the disk was replaced"; a replaced disk joins as an
observer and the operator adds the new (id, UUID) and removes the old one, one voter change at a time ("For safety
Kafka will only allow one voter change operation at a time"). OpenRaft states the same rule: "A node ID must identify
the same node, holding the same log, for the entire lifetime of the cluster", and reusing an id on an empty log "can
split the cluster into two independent quorums" [EXT-SRC: OpenRaft `docs/faq/faq.md`].

[INF] A rule that lets the decider choose an invalid membership and relies on the node to refuse later leaves a window
in which the chosen record is wrong, and every reader of the record (promotion, R7, acknowledgement counting) must
know about the exception. Rejecting at the decider removes the window. **Under BUILD** the witness compare-and-set
should check the incarnation (P3 of the live-grant package already leans this way), and LG-F3's node-side refusal
stays as defence in depth.

**Under Raft** (corrected after the independent review): there is no decider separate from the voters, and raft-rs
messages carry no incarnation. A wiped node under its old id answers votes from an empty `HardState`, and the leader
cannot tell it from a slow follower. So the primary guard is at the node: a node never runs a Raft id whose
`HardState` and incarnation record are missing; it joins as a learner under a new id, and the old id is removed by a
configuration change. The second guard is SNC's transport: every message carries (replica id, incarnation), and a
receiver drops any message whose incarnation does not match the one it knows. Kafka's KIP-853 (voter = id plus
directory UUID) is the precedent, though it is written for KRaft metadata voters, not data replicas.

**Neither guard covers a lost tail under the same id** (a lost write, FM-3, or FM-9): if follower F persisted entry e,
e committed on {leader, F}, F silently loses e, and the leader then fails, F and the third voter can elect a leader
without e. That is D-02's job under Raft: a voter whose log tail cannot be proven must not vote or acknowledge until
it re-proves it from peers (CTRL, FAST '18). RX-1 tests this case.

## 4. Candidates

### 4.1 Custom SNC (BUILD)

- State [FACT: PR #180 docs, docs/handoff/PR179_Owner_Decisions.md]: L-1 real and unresolved; A-1 and LG-F1..F4
  proposed; L1Authority.tla and LiveGrant.tla not composed; the full LiveGrant lanes "never complete" at 2 h caps;
  Y9-T blocked; D-02, R-23, O-FLOOR open. Codex: final acceptance needs "a composition argument or targeted integrated
  model covering authority, membership, replication acknowledgments and destructive state transitions".
- Remaining work [INF]: the composition model, the counted campaigns, then C++ conformance, then a Rust
  implementation of the protocol, the witness service and their tests. None of it is shared with any other system,
  so every bug is ours to find.
- At three data copies (A ⊇ S, R2′) BUILD keeps writes through one data-node loss, at the same hardware cost as
  Raft; that layout is the fair comparison, and it still carries every open item above.
- Its strengths: two data copies when cost matters more than write availability; the D-01/R9 machinery already designs standby reads under leases; FM-1..16 work is
  sunk but **reusable under every option** (section 6).

### 4.2 raft-rs (HYBRID core, recommended)

- **Status** [EXT-SRC]: crate 0.7.0 (2023-03-07), Apache-2.0; latest master commit 2026-05-13. The independent review
  counted 19 master commits since 2024-01-01 and 8 since 2025, mostly lint, typo and dependency work: low-velocity
  maintenance driven by TiKV's needs, not active development. TiKV master depends on it through a git patch:
  "# TODO: remove this when new raft-rs is published." So production runs master, not the release. If adopted, SNC
  names an owner for the pinned rev and accepts that it may have to maintain a fork. Lineage: files carry "Copyright 2015 The etcd Authors"; the design follows etcd raft.
- **Durability obligations** [EXT-SRC `src/lib.rs`, `src/raw_node.rs`]: append `entries` and persist a changed
  `HardState` before acting on them; a follower's messages are sent "AFTER the HardState, Entries and Snapshot are
  persisted to stable storage"; "Ready must be persisted in order"; persist the commit index "with or before applying
  entries". Async persistence through `advance_append_async` / `on_persist_ready`.
- **Features** [EXT-SRC `src/config.rs`, `src/read_only.rs`]: pre-vote and check-quorum (both off by default; we would
  turn both on), `ReadOnlyOption::Safe` (ReadIndex, no clock) by default and `LeaseBased` ("can be affected by clock
  drift"), joint consensus (`ConfChangeV2`), learners, flow control (`max_inflight_msgs`), election priority, leader
  transfer.
- **What SNC must build** [INF]: the log storage (SNC's WAL stream, whose `(term, LSN)` per record is already the D-06
  direction), `HardState` persistence (replaces the node metadata record), checksums and fail-closed refusal (raft-rs
  does none; TiKV's raft-engine adds a crc32 per batch and `Error::Corruption` [EXT-SRC]), snapshots (the checkpoint,
  with the applied index written atomically with it: etcd's v3.5 consistent-index bug is the warning,
  docs/prior_art/ETCD_CONSISTENT_INDEX.md), snapshot transfer (today's seed), transport and batching, tick driving on
  the shard thread, leader placement, client routing to the leader, operator unsafe recovery (TiKV keeps its in PD:
  "Online Unsafe Recovery is a type of lossy recovery" [EXT-WF]), and a deterministic simulation harness.
- **Risks**: release cadence (mitigation: pin a reviewed rev; Apache-2.0 allows vendoring); the integration surface is
  still ours (the etcd bug shows it is where mature systems fail); Jepsen's TiDB 2.1.7 report found its anomalies in
  the SQL layer's retries, not Raft, but did note under-replicated new regions [EXT-WF: https://jepsen.io/analyses/
  tidb-2.1.7], which is an operational rule, not a library property.

### 4.3 OpenRaft (HYBRID alternative, fallback)

- **Status** [EXT-SRC, crates.io API]: 0.10.1 (2026-11-19), MIT or Apache-2.0; "Upgrades before `1.0.0` may include
  incompatible changes"; used by Databend's meta-service and others.
- **Strengths**: more is built in: snapshot building, chunked install (`snapshot_max_chunk_size` 3 MiB), purge rules
  ("Logs that are not in a snapshot will never be purged"), membership API with joint consensus, learners, flexible
  quorums, `ensure_linearizable` with ReadIndex or lease. Testing is strong: "A deterministic simulation fuzzer runs in
  every CI build. The Jepsen suite runs 8 nemesis scenarios on every push to `main`" (README).
- **Weaknesses for SNC**: it schedules its own tasks (3.3), although `openraft-rt-monoio` and `openraft-rt-compio`
  (thread-per-core io_uring runtimes) and the `single-threaded` feature, under which `Raft` is not `Send`, let those
  tasks stay on the shard's core [EXT-SRC, per the independent review: `openraft/Cargo.toml`]; pre-vote "is disabled by default"; storage traits require the
  same obligations ("The vote must be persisted on disk before returning"; reordering vote and log I/O "can lead to
  committed data loss") and give no corruption detection; the 0.10.0 change log lists recent safety fixes: a restarted
  leader could reuse a log id for different entries (#2167), two leaders could commit disjoint membership configs, a
  state machine missing purged logs was accepted at startup (#2222) [EXT-SRC `change-log/v0.10.0.md`]. They are fixed;
  they also show the newer implementation is still converging. Concurrent `change_membership` calls "can fail midway
  or leave a joint config" (FAQ).
- **Compared with raft-rs** [INF]: OpenRaft is less glue and more tested in CI; raft-rs is a better fit for per-core
  execution and a longer production lineage. Fit decides it for SNC, because the runtime model is the thesis.

### 4.4 PostgreSQL 18 + Patroni (ADOPT as the store: not recommended)

- **Guarantees** [EXT-WF]: with `synchronous_commit = on` a commit waits until standbys have "flushed it to durable
  storage"; `ANY n (...)` gives quorum commit; Patroni promotes only a synchronous standby and keeps state in a DCS
  (etcd, Consul, ZooKeeper or Kubernetes). PostgreSQL 18 enables data checksums by default at `initdb`.
- **Caveats** [EXT-WF]: Patroni: "Turning on synchronous_mode does not guarantee multi node durability of commits under
  all circumstances"; a backend cancelled while waiting leaves "transaction changes ... visible for other backends";
  PostgreSQL: "If primary restarts while commits are waiting for acknowledgment, those waiting transactions will be
  marked fully committed once the primary database recovers." Those are G-11 and G-15 gaps unless every client treats a
  cancelled or interrupted commit as in doubt.
- **Fit** [FACT: docs/REPLICATION.md, sync-replica table]: with a synchronous replica, SNC did 640K ops/s and
  PostgreSQL 29K, "about 22×" (docs/CONVENTIONAL.md calls the in-memory comparison "not a like-for-like database
  comparison"). Every mutation would leave the owning core for a SQL
  transaction, so objects become a cache over rows: ownership (invariant 3), locality (4) and state-native execution
  become an ORM. Durable tasks and timers would be tables plus polling or `NOTIFY` [EXT-WF].
- **Verdict**: unsuitable as the authoritative state layer. It remains a fine export or integration target.

### 4.5 FoundationDB (ADOPT as the store: not recommended; keep as a reference)

- **Design** [EXT-WF, SIGMOD 2021 paper https://www.foundationdb.org/files/fdb-paper.pdf and
  https://apple.github.io/foundationdb/architecture.html]: a commit is acknowledged "Only when all k have replied with
  successful persistence" (f+1 log servers); each epoch "represents a generation of the transaction system"; recovery
  locks the coordinated state and stops the old log servers; recovery is "a few hundred milliseconds" typically (docs),
  median 3.08 s in the paper's production data. It is not a drop-in library: it is a whole cluster with coordinators,
  a cluster controller, sequencer, proxies, resolvers, log and storage servers.
- **Limits** [EXT-WF, known-limitations.html, api-general.html]: transactions over five seconds are unsupported; 10 MB
  of affected data; values at most 100,000 bytes; "A client process can have only one network thread for the entire
  lifetime of that process". The Rust binding is community-maintained.
- **Fit** [INF]: every SNC mutation would become a remote optimistic transaction; per-core ownership becomes a cache
  with conflict retries; the single client network thread works against per-core shards.
- **Verdict**: unsuitable as a backend. As a reference it is valuable: generations that fence the whole old
  configuration at once, and deterministic simulation as the way an f+1 design is made correct.

### 4.6 Kafka and Redpanda (prior art only)

Taken above: decider-side eligibility (KIP-903, KIP-966: ELR is "maintained purely on the controller side"),
maximal-ISR authority while a change is pending (KIP-497: "When expanding the ISR, the leader will assume the larger
set immediately"; on shrink it "will continue relying on the previous ISR until" the change is confirmed, which is
A-1's shape), storage identity per voter (KIP-853), one membership change at a time, leader-epoch truncation (KIP-101,
KIP-279). Redpanda: Raft per partition, learners "not accounted to the majority", voter lists never shrunk "to a size
smaller than the requested replication factor", and `acks=all` flushing on the majority before acknowledging, with
write caching as an explicit weaker mode [EXT-WF: redpanda.com blog posts on reconfiguration and write caching]. That
last pattern is how SNC should keep any `m = 1`-like speed mode: named, per group, never the default.

## 5. Decision matrix

Columns are the same requirements for every candidate. "Changes" names the SNC text that would have to change.
The custom column describes today's two-copy layout; at three data copies (4.1) its rows 1, 5 and 11 move to match
the Raft columns on copies, write availability and cost, and every other row stays as written.

| # | Requirement | Custom SNC (BUILD) | OpenRaft (HYBRID) | raft-rs (HYBRID, recommended) | PostgreSQL + Patroni (ADOPT) | FoundationDB (ADOPT) |
|---|---|---|---|---|---|---|
| 1 | G-11, G-12: HA_DURABLE, admission | Meets by design with 2 copies; writes stop on any single data-node loss; proof incomplete (L-1, composition) | ≥ 2 copies with 3 voters; admission = quorum reachable. Changes: G-11 [PROPOSED] counting text ("members of S" → "a quorum of voters"); G-12 wording ("in-sync set" → quorum) | Same as OpenRaft | `on` + `ANY 1`: ≥ 2 copies, but cancelled waits and restarts expose or keep unreplicated commits. Changes: G-11 (in-doubt class), G-12 | ≥ 2 log copies in double mode; commit result can be unknown (retry). Changes: G-11 class semantics, G-12 reporting |
| 2 | G-14, G-15: no silent loss, log identity | G-15 by R6.0/R6.3′ rules; G-14 needs D-02 (open) | G-15 = Raft Log Matching; G-14 needs SNC storage checksums + D-02 (library has none) | Same; raft-engine shows the per-batch CRC pattern | Checksums default in 18; identity = WAL LSN + timeline; G-15's reuse rules replaced by timelines | Internal; SNC loses visibility and control of the log |
| 3 | G-35: operator promotion, weaker guarantees | R7 + explicit data-loss acknowledgement (designed) | No unsafe recovery built in; SNC builds "force new config from survivor" with the same acknowledgement | Same; TiKV's Online Unsafe Recovery is the model | Patroni manual failover; data loss possible outside sync mode | Operator-forced recovery exists at cluster level only |
| 4 | G-48: recovery without operator | Designed; two crash-only refusals unresolved (FM-1) | Automatic after crash; G-14 refusals need an operator (unchanged) | Same | Automatic; pg_rewind off by default | Automatic, seconds |
| 5 | `m = 2` and quorum size | 2 data + 3 witnesses; `m` exact | 3 full voters, quorum 2; `m = 1` = 1-voter group. Changes: D-03 (a) → (b) | Same | 1 primary + 2 sync standbys for availability | double/triple mode, 3-5 coordinators |
| 6 | Exclusive authority, fencing | Term leases granted by A, witness majority when degraded (D-01); L-1 open | Terms + leader lease + CheckQuorum; ReadIndex without clocks. Changes: D-01, G-33 mechanism text | Same, with pre-vote and check-quorum switched on | DCS leader key TTL (30 s default); failsafe mode | Generations; old log servers locked at recovery |
| 7 | Rejoin, membership changes | CAS on witness record, LG-1..6, A-1, R1.1; LG-F1..F4 open | Joint consensus, learners; id must never be reused on an empty log; concurrent changes can stick in joint config | Joint consensus (`ConfChangeV2`), learners, one change at a time by SNC | Patroni rejoin, rewind or re-clone | Automatic re-recruitment |
| 8 | Checkpoint, snapshot, log recovery | SNC checkpoints + seed; O-FLOOR open | Built-in snapshot policy, chunked install, purge rules | SNC checkpoint = snapshot, seed = transfer; applied index with checkpoint | Base backup + WAL | Internal |
| 9 | Crash consistency, corruption detection | FM-1..16 contract (proposed) | None in library; FM contract still needed | None in library; FM contract still needed | Page checksums, WAL CRC | Internal |
| 10 | Rust, licence, maintenance | Ours entirely | Rust, MIT/Apache-2.0, active, pre-1.0 API churn | Rust, Apache-2.0, low-velocity TiKV maintenance, no release since 2023 (git pin, possible fork) | C server; PostgreSQL License, Patroni MIT; client crates | C client + community Rust crate, Apache-2.0 |
| 11 | NUMA locality, latency, throughput, overhead | Best fit; 2 copies | Runtime owns scheduling; per-group tasks; 3 copies | Shard thread drives it; per-core groups; 3 copies (+50% RAM/disk/apply, 2x leader egress) [HYP: commit latency equal or better, faster of two followers] | Remote SQL per op; about 1/22 of SNC's sync-replicated throughput [FACT: REPLICATION.md] | Remote transaction per op; ms commits; one network thread per client |
| 12 | Integration effort, complexity, testing | Composition model + campaigns + C++ conformance + Rust protocol + witness service; open-ended | Storage + state machine + network adapters; less glue | Storage, snapshot, transport, ticks, routing, unsafe recovery, simulation harness; bounded and well documented | Rewrite the object layer over SQL | Rewrite the object layer over FDB; run a cluster |

## 6. What survives from AFR-001 under the recommendation

[INF] Kept as SNC's own work, because no Raft library does it: the FM-1..16 fault model; G-14(a) fail-closed recovery
and the refusal matrix; D-02 (whichever mechanism, now as "never vote or campaign on a log whose tail may have been
lost"; CTRL, FAST '18, already in docs/AFR_001_DURABILITY_CONTRACT.md 8.1, is the protocol-aware recovery prior art);
R-23 (metadata rollback becomes `HardState` rollback, a double-vote hazard); D-06's per-record terms (become Raft
entry terms); O_DIRECT rules; standalone LSN reservation for `m = 1` single nodes; the consistent cut and causal IDs of
docs/FUTURE_ARCHITECTURE.md.

Retired: everything in section 1's list. The Combined.tla, L1Authority.tla and LiveGrant.tla work stays recorded as
evidence of why. Raft itself needs no new SNC model. The composition still needs an argument and tests: per-shard
groups against node-wide state (the node metadata record, R6.5), against placement and slot moves on the witnesses,
and the integration points (storage obligations, snapshot and applied index, identity on disk replacement), by
simulation and fault injection (section 8).

**Mapping of SNC's node-wide and per-op constructs** [INF, from the independent review]:
- **One atomic metadata authority per node** (PR155 condition): 36 independent `HardState` files would break it. Keep
  one node-wide record that holds every shard group's `HardState`, written atomically, as TiKV's raft-engine keeps all
  regions in one log (docs/prior_art/COCKROACHDB_TIKV_TRUNCATION.md part B).
- **Atomic WAL groups, outbox, sessions** (O-1, invariant 18): one Raft entry = one WAL group, so the mutation, its
  outbox entry and its session record commit or vanish together. Deduplication state is part of the shard state and
  is rebuilt with it after a deposed-leader truncation.
- **Mint leases** (E11): a new leader must hold a fresh mint lease block before it serves, a readiness step after
  election (like Raft's no-op entry of its term); SNC-built.
- **Recovery barrier and epoch adoption** (invariants 16, 17): a shard serves only when its group has a leader that has
  committed an entry in its own term, has applied to the commit index, and has adopted the node's placement epoch.
- **PE-3**: an election timer ticked only by a busy shard thread makes a busy shard look dead. Ticks and heartbeats need
  a bounded path that long work cannot starve, or a shard loop with bounded work per turn; SNC-built and measured in
  RX-1.
- **PE-4**: Raft's vote restriction (up-to-date log) is its eligibility rule; SNC still adds readiness (mint lease,
  barrier) before a leader serves.
- **Standby reads** (G-30): lease-based standby reads (R9) go away; follower reads use ReadIndex from the leader, or a
  bounded-staleness class if one is wanted. Whether SNC keeps standby reads at all is cody's call.

## 7. Decisions this needs from cody, and the rules it revises

Accepting HYBRID would revise:

1. **AFR-D-03** direction (a) "witness-coordinated in-sync replica set" → (b) majority-acknowledged replicas. The
   owner's own condition was "Do not switch to majority-acknowledged data replicas without a separate architectural
   review"; this document is offered as that review's input.
2. **AFR-D-01** term-scoped grant leases → Raft terms with leader lease and ReadIndex. The clock-drift assumption stays
   for lease reads only.
3. **R2′ and R9-A..E** (conditionally frozen 2026-11-17) and **OD-10, OD-15** → superseded, not edited.
4. **G-11 [PROPOSED] counting text, G-12, G-33, G-35** mechanism wording → restated for voters and quorums. The
   **accepted** meaning (≥ 2 durable copies for HA_DURABLE; `m = 2` default; `m = 1` per group and LOCAL_DURABLE)
   is preserved.
5. **Replica count**: three data copies per group by default instead of two. A cost decision.
6. **Invariant 14** ("the hot execution path ... does not perform consensus"): a quorum commit per write is
   consensus in the write path. This is an owner revision of the invariant, not a clarification: today's sync
   acknowledgement is one replica's round trip, not a quorum decision.
7. **Standing rule on prior art.** The rule "take ideas, do NOT adopt or embed those systems; build SNC's own
   version" (cody, 2026-11-16) is recorded in project memory and the coordinator's brief, **not in the repository**;
   the repository's version is docs/ENGINEERING_PRINCIPLES.md ("Do not mechanically copy another system's
   implementation", "Do not introduce external dependencies ... without authorization"). Either way the
   recommendation needs an explicit exception for the consensus core. AGENTS.md invariant 6 names storage libraries
   as replaceable, but a consensus library that brings its own replication abstractions is not obviously covered, so
   this is an owner revision too. The rule to stay independent of Seastar, DPDK and VPP is not affected.
9. **Other texts whose mechanism wording moves** (meaning kept unless cody says otherwise): G-30 (standby reads, see
   section 6), G-31 and G-34 (one serving owner, role reporting: per group and per term), G-32 (newer term learned
   durably: Raft's `HardState`), G-14(a)'s term-history wording and the PR157 decision (term history from per-record
   WAL terms becomes Raft's log terms), PR155's node-wide atomicity condition (kept, section 6), R6.2, R6.3′, R6.5 and
   E1-E11 (replaced by Raft's log matching, vote restriction and the readiness steps of section 6), PE-3 and PE-4
   (section 6), D-06 items 4-14 (per-record terms stay; the reconciliation-specific items go), G-48 (unchanged in
   intent), and invariants 11-13 (a replication group becomes a set of per-shard Raft groups on the same nodes;
   failover is still not migration; placement epoch and Raft term stay separate) and 15-18 (unchanged, mapped in
   section 6).
8. **SNC-ADR-RUST-001 timing**: the experiment below is Rust code before the readiness review. It needs cody's
   explicit go as an isolated spike, not production work.

Until cody decides, nothing changes: the other thread's `Combined.tla` fix and L-1 work proceed under Codex's order,
and no broad LiveGrant campaign should be requested before this decision (Codex's section 8, relayed text).

## 8. Smallest isolated integration experiment (proposal only, not authorized)

**Step 0, free: cody's cost answer.** Is one extra full replica per group (about 1.5x RAM) acceptable to start, with
the log-retaining voter of 3.1 as the follow-up? If not, RX-1 starts with that layout instead, or the answer is BUILD.

**RX-1: one replication group on raft-rs.** Goal: falsify the recommendation cheaply. It does not re-test Raft itself;
it tests the integration points where SNC would own the risk.

- **Scope**: a standalone Rust crate outside the C++ tree; raft-rs pinned to one reviewed rev with
  `max_apply_unpersisted_log_limit = 0`, pre-vote and check-quorum on; SNC's record format for entries (full post-op
  images); a minimal checkpoint as snapshot; an in-memory object map as the state machine; three replicas in one
  process under a deterministic simulator, then three processes. No placement, migration, wire change or R640.
- **Gates, in order** (each preregistered, each a stop point):
  1. **Execute before commit**: a leader runs operations ahead of commit, is deposed with uncommitted entries, and
     rebuilds; no acknowledged or read state is ever lost or invented.
  2. **Lost tail under the same id**: a follower silently loses a committed tail entry, then the leader fails; the
     follower must refuse to vote until it re-proves its tail. Also a wiped disk under the old id (must not rejoin as
     that id) and a shrink while the leader is partitioned (L-1's shape).
  3. **Shard-loop fit and latency**: drive the group from a pinned shard-style loop with batching; measure p99
     acknowledgement latency and throughput per shard. The comparison with today's C++ sync standby is confounded by
     language and is reported as such; the threshold is set against a same-crate no-replication baseline and fixed
     before results (the owner's 10% D-02 limits are the template). Repeat the loop with OpenRaft `single-threaded` on
     the same harness to settle 4.2 versus 4.3.
  4. **36-group node kill**: election time, leader scatter, and time to transfer leaders back.
- Checker throughout: acknowledged set ⊆ recovered set or a named refusal; one serving leader per term; linearizable
  reads.
- **Outcome**: GO (adopt the direction and plan the Rust readiness review around it), REFINE (OpenRaft, a different
  mapping, or the log-retaining voter), or STOP (back to BUILD with this evidence). A timeout or crashed run is
  UNRESOLVED, not a pass.

## 9. Attempts to disprove the recommendation

Each challenge was put to the recommendation; unresolved ones say so.

1. **"Three copies is a 50% cost increase for an in-memory system."** True. It is only an increase relative to a
   two-copy group that stops writing after one failure. If cody values two-copy cost over write availability, BUILD
   is the answer and the cost is the protocol. **Owner's call, unresolved by analysis.**
2. **"Raft does not remove the hardest storage work."** True: D-02, R-23 and the FM contract remain (section 6). The
   recommendation removes the distributed protocol, not single-node durability.
3. **"Integration bugs are where mature Raft systems fail"** (etcd's consistent index, OpenRaft's log-id reuse).
   True; the integration surface is named in 4.2 and is the target of RX-1's simulator. It is a documented, shared
   surface, unlike the custom protocol's.
4. **"Per-shard groups can put leaders of one replication group on different nodes."** True; routing is per shard
   already, and leader priority plus transfer can keep them together. Whether to require co-location is open
   (**unresolved**, measured in RX-1).
5. **"Leases depend on clocks."** Only lease reads; ReadIndex is the default and needs no clock. Same assumption D-01
   already accepted.
6. **"raft-rs has no release since 2023."** True; TiKV runs master. Pinning and vendoring are allowed by its licence.
   If maintenance stops, the cost is owning a small sans-IO core, still less than owning a new protocol. **Residual
   risk, accepted only by cody.**
7. **"Cross-shard obligations need a cut across groups."** Same problem under every option (FUTURE_ARCHITECTURE
   section 2); Raft neither helps nor hurts.
8. **"SNC is C++ today; raft-rs is Rust."** Production is planned Rust-native (SNC-ADR-RUST-001), so a Rust consensus
   core matches the target and the C++ reference would not link it. C++ libraries (eBay NuRaft, Baidu braft) and etcd
   raft (Go, needs cgo) were not researched: they do not fit a Rust production target. **Residual: the C++ reference
   would no longer share the production replication design, which weakens differential testing of this layer.**

## 10. Limits of this evidence

- KIP pages, PostgreSQL and Patroni docs, FoundationDB docs and paper, Redpanda pages and the papers were read through a
  summarising fetch tool: quotes are not byte-verified. docs.rs, docs.pingcap.com, GitHub issue pages and the GitHub
  API returned 403; raft-rs and TiKV facts come from source and the pingcap/docs markdown instead.
- UNVERIFIED: a one-in-flight rule stated in KIP-497 itself (KIP-853 states one for voters); what exactly a TiKV
  witness stores; a documented TiKV rule that a data-lost store cannot reuse its id; PostgreSQL 18's exact wording on a
  cancelled synchronous wait (Patroni states it); FoundationDB's per-mode log replica counts and its checksums;
  Redpanda's handling of a replica restarting with missing data and of snapshot/compaction boundaries.
- No performance number for any Raft option on SNC's workload exists. Section 5 row 11 is HYP until RX-1.

## 11. Independent review

Two independent reviewers were asked to break this document: one against SNC's own texts, one on distributed-systems
grounds with access to raft-rs and OpenRaft source.

**Distributed-systems review (Opus, completed).** Eleven defects; all were accepted and applied:
execute-before-commit and deposed-leader rebuild (3.3, RX-1 gate 1); `max_apply_unpersisted_log_limit` (3.3); D-02's
Raft scope and the lost-tail-under-same-id case (3.4, gate 2); the decider direction inverted for Raft (3.4);
OpenRaft's thread-per-core runtimes (1, 4.3, gate 3); raft-rs maintenance numbers (4.2); language and FFI (9.8); the
log-retaining voter (3.1); hidden multi-group costs (3.3, gate 4); the headline led by protocol risk, not `m = 2` (1);
and a smaller, ordered RX-1 with a free step 0 (8). Its claims about raft-rs PR #537/#561 and commit counts were not
re-checked by the author.

**SNC-texts review (Sonnet, completed; an earlier Opus run stopped on a usage limit before reporting).** Nine
defects, all accepted and applied: provenance of Codex's review (header); the prior-art rule is not in the repository
(7.7); PR148's `m = 2` is section 3, with its "do not silently weaken" caveat (1); BUILD compared fairly at three data
copies (1, 4.1); section 7's omissions (7.9: G-30 to G-34, E11, PE-3, PE-4, PR155, PR157, D-06, invariants 11-18);
node-wide constructs, WAL groups, sessions, mint leases and the recovery barrier mapped (6); the witness Paxos stays
for placement (1); the PostgreSQL ratio relabelled to the like-for-like 22x (4.4, 5); invariants 6 and 14 framed as
owner revisions (7). Its point that the effort comparison is unestimated is stated in 1.

**Unresolved after review:**
- Whether G-11 counts durable copies at acknowledgement only or continuously: after one voter is lost, writes
  committed on {leader, lost voter} sit on one live copy until catch-up. The same is true of the custom protocol after
  a member loss; the accepted text says `m` "controls admission, not the number of copies of earlier writes".
- Whether "reads wait for durability" carries over to ReadIndex without exposing leader-side uncommitted state.
- Whether replacing a voter by remove-then-add (3 → 2 → 3) is allowed; add-then-remove avoids the two-voter window.
- Leader co-location across a replication group's shard groups (9.4).
- Whether cody accepts the invariant 14 clarification and the prior-art exception (section 7).
- 1.5x RAM versus the log-retaining voter (step 0).
- Whether BUILD at three data copies beats Raft at three: not evaluated beyond the open items of 4.1.
- Whether standby reads (G-30) are kept.
- Whether reopening AFR-D-03 toward (b) is accepted; PR148 asks for a separate architectural review, and this document
  is only its input.

