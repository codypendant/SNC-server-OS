# SNC-AFR-001: owner-decision package

- **Status: PROPOSED, for cody's decision. Nothing in this document is approved.** D-01, D-03 and the default
  minimum set size are recommendations from Codex and Claude, not owner decisions. They stay unaccepted until cody
  confirms them in their own words.
- **Trigger.** Codex's review of PR #147 (`docs/handoff/PR147_Guarantee_Fixes_Codex_Review.md`), section 4:
  "Prepare a concise owner-decision package."
- **Inputs.**
  - The contract text: `docs/SNC_GUARANTEES.md` (SG) and the protocol in `docs/AFR_001_SAFETY_ANALYSIS.md` (SA).
  - The design comparison: `docs/AFR_001_CORE_DECISIONS.md` (CD).
  - The evidence and its limits: `docs/AFR_001_PROTOCOL_ACCEPTANCE_REVIEW.md` (AR) and `spec/tla/RESULTS.md` (RES).
- **Scope.** No new modelling and no runs. One new primary source is cited, Kafka's `min.insync.replicas` page.
  X-2 is not started, and nothing here authorizes C++ or Rust work.
- **Review.** An independent adversarial QA found 2 blockers, 8 majors and 4 minors in the first draft. All are
  addressed here (section 8).

## 0. What cody is asked to decide

| # | Decision | Recommendation | Section |
|---|---|---|---|
| 1 | Conditionally accept **AFR-D-01**: term-scoped primary leases | **Accept, with the conditions in 1** | 1 |
| 2 | Conditionally accept **AFR-D-03**: an in-sync replica set under a witness-held, versioned group record | **Accept, with the conditions in 2** | 2 |
| 3 | Default **minimum set size** `m` | **`m = 2`**. `m = 1` only as an explicit per-group setting with its class reported | 3 |
| 4 | **AFR-D-02**: end-of-log rule | **Keep on HOLD** | 4 |
| 5 | Preregistering X-2 (no run) | **Authorize the preregistration only**, so cody can set its criteria | 4 |

**What "conditionally accept" means:**
- The protocol text in SA (R1-R9) and the clauses in SG become the **normative contract** the Rust architecture is
  designed against.
- That happens only once the conditions listed under each decision are met, and with the stated assumptions.
- Changing the contract afterwards takes a recorded revision with new evidence.
- It is not a proof: every model result is bounded (AR header).
- **SA supersedes CD where they differ.** CD 5.1 takes the lease from S and lets degraded mode run without one. SA
  takes it from A, uses a witness lease in degraded mode, and replaces CD's restart wait with R3's `learned + W`.

**Why D-01 and D-03 can be accepted without D-02.** CD 6 advised against accepting the three separately. The
reason given was that choosing majority quorums for D-03 would change every coupling. With D-03 as recommended,
those couplings are fixed:
- the set grants the leases;
- the term fences authority and positions records.

D-02 then decides only the single-copy boundary (G-14(b)) and byte formats. One consequence must be named: SA's
model needs each WAL record to carry its term. That is a format change accepted with D-01 and D-03, and its bytes
wait for AFR-D-06.

**Three separate decisions. This package asks only for A.**

| Decision | Meaning | Status |
|---|---|---|
| **A. Accept the normative contract** | SG and SA text binds the design | Asked now |
| **B. Verify the C++ reference** | Show the C++ meets the accepted contract (SG section 15) | Not asked. The C++ has no primary lease, no durable fencing and no sets of 3 or more members (G-31 PARTIAL, G-32 VIOLATED, G-33 ABSENT) |
| **C. Authorize Rust implementation** | Start writing crates | Not asked. Gated by SNC-ADR-RUST-001 and the readiness review. Section 5 lists the remaining design blockers |

## 1. AFR-D-01: authority model

**Recommendation: conditionally accept term-scoped primary leases**, as specified in SA R3, R4, R5, R8 and
"Ack set A and lease". cody chose the lease direction on 2026-11-16. This decision would make it the contract.

**What would be accepted:**
- **Normal mode.** A primary acknowledges or answers default reads only while it holds a lease for its own term
  from **every member of A**. A member that has not granted counts as expired (SA "Ack set A and lease").
- **Degraded mode** (S = {p}). The lease comes from a majority of witnesses under W1 (R8).
- **New primary.** It serves only after `learned + W` (R3).
- **Fencing.** A node that learns a newer term persists it first and never serves the old term again, including
  after a restart (R4, G-32).
- **Answer time.** Authority is checked when the answer is produced (R5). Status reports only provable authority
  (G-34).
- **Guarantees:** G-28, G-31 (two primaries of one group), G-32, G-33, G-34 and G-46, as now worded.

**Assumptions the contract states and the implementation must check:**
- each clock runs at a real rate in `[1-ρ, 1+ρ]`, with ρ a stated number;
- the clock is suspend-aware (`CLOCK_BOOTTIME`). VM pause and drift beyond ρ are outside the contract (G-46);
- fencing reports success only when the node has stopped, and members read the group record on boot (R7);
- witness reads are linearizable (A9).

**Evidence (bounded):**
- **Checked:**
  - `OneAuthority` holds in every completed Authority and Combined run;
  - removing the barrier, W1, the grant term check or the learn step-down each produces a counterexample (AR
    section 2).
- **Argued only:**
  - R4's durability, which was never removed in any run;
  - R5 and all reads;
  - G-34.
- **UNRESOLVED:** whether R2's READY timing rule is needed (AR section 2).
- **Not covered:** no completed run combines a restart, fencing, operator promotion and standby leases (AR
  section 4, item 5).

**Conditions before acceptance takes effect:**
- SG section 14 is updated: the D-01 row states the decision, and CD 5.1 is marked superseded by SA.

**Obligations that follow acceptance and block later steps:**
- A7 and A9 move to the consensus-service design (AFR-D-13);
- lease and timeout lengths, and the cost of renewal and of the barrier (X-3, which needs a prototype);
- the default read level (AFR-D-10), which depends on this decision.

## 2. AFR-D-03: replication group model

**Recommendation: conditionally accept** the in-sync replica set coordinated by witnesses (CD section 3, option A):
- every member of the set acknowledges every write;
- the witnesses own a versioned group record (term, primary, set, mode, `m`, READY), changed only by
  compare-and-set;
- do not move to majority-acknowledged replicas.

**Why:**
- Two copies tolerate one failure; a majority quorum needs three.
- It is the shape of PacificA, Vertical Paxos II, chain replication, FoundationDB's log servers and Kafka's ISR
  (CD section 7).
- In the C++, what is tested is the exclusion it rests on (F-3), for pairs under process kill. Host crash is
  UNPROVEN (G-13).

**Membership and promotion rules that would be accepted:**
- **R1.** A ⊇ S. A member joins A before S. It leaves A only after a shrink is chosen, or after a read of the
  record at the primary's own term shows it out of S.
- **R2.** Automatic promotion is only from S, and the new S = {j}. READY is recorded after the barrier.
- **R6.** Reconciliation by (term, LSN), with a durable truncation intent, reseed and a durable incomplete flag. A
  damaged term history refuses (G-14(a)). G-15: the same (term, LSN) never identifies different content.
- **R7.** Operator promotion carries the expected term.
  - Inside S: allowed.
  - Outside S: only from a degraded, READY record, or with every member of S confirmed fenced. It must disclose
    possible loss of acknowledged writes (G-35).
  - Case 5c is not a protocol operation.
- **R9.**
  - A standby lease never outlasts the primary's authority. This is model-checked (S_M2).
  - The primary does not degrade, drop a standby from A, or acknowledge without it while the standby's lease may
    still be valid. This is G-30's clause and is not modelled.

**Conditions before acceptance takes effect:**
1. E1-E11 (promotion eligibility) are rewritten for an in-sync set and reviewed. G-35 depends on them, and they do
   not exist yet (AR section 3).
2. SG section 14 is updated: the D-03 row lists the in-sync set for N ≥ 3 as the chosen option.

**Obligations after acceptance:**
- membership change, for replicas and for witnesses (AFR-D-11, AFR-D-13);
- R9's G-30 clause needs model coverage (Codex obligation 2);
- the slowest member sets write latency, and catch-up time sets the single-copy window. Both are unmeasured (X-1);
- host-crash durability (G-13) is unproven.

## 3. Minimum in-sync set size `m`

`m` controls **admission only**: whether SNC acknowledges a new write. It never adds copies of writes acknowledged
earlier (SA section 3).

| Situation | `m = 2` (recommended default) | `m = 1` |
|---|---|---|
| Any failover | Nothing is served until `learned + W` (R3). Then reads resume. Writes are refused until a member catches up and the grow is chosen, **even with three replicas**, because R2 sets S = {j} | Reads and writes resume after `learned + W`. Writes are acknowledged on one copy |
| Pair loses its non-primary member | Reads and writes stall until the degrade is chosen (R9 first waits out that member's standby lease) and a witness majority grants. Then reads continue and writes are refused until a replacement catches up | Same stall. Then writes continue on one copy |
| Three replicas lose a non-primary member | Reads and writes stall: that member is still in A, and its lapsed grant expires the lease. They resume once the shrink to a set of 2 is chosen (R9 waits out its standby lease first). This is not degraded mode | Same |
| Writes acknowledged before the failure | One copy until a member catches up | Same: `m` does not change it |
| Class reported for a one-copy write | No such write | LOCAL_DURABLE, never HA_DURABLE (G-12) |

**Recommendation: `m = 2` by default. `m = 1` only as an explicit per-group setting whose weaker class the client
sees.** This is also Codex's recommendation.

**Kafka is a reference point, not a match.** Kafka's documentation describes "a typical scenario" of "a replication
factor of 3, set `min.insync.replicas` to 2", with producers using `acks` "all", who get `NotEnoughReplicas` when
the in-sync set is smaller [EXT: kafka.apache.org/44/configuration/topic-configs, `min.insync.replicas`]. Kafka's
default is 1. The difference that matters: Kafka keeps surviving followers in the in-sync set when it elects a new
leader, so that setup keeps taking writes after a failover. SNC's R2 empties the set to {j}. **Under `m = 2`, every
SNC failover is therefore a write outage that lasts as long as catch-up takes.** That duration is unmeasured (X-1).
Keeping members whose log is a prefix of the new primary's in S would shorten it. That idea is unanalysed [HYP] and
not proposed.

**Condition for decision 3:** amend G-11. With S = {p}, "acknowledged by every automatically promotable replica" is
vacuously true. HA_DURABLE must require |S| ≥ 2 (SA section 3). Until G-11 says so, the `m = 1` column does not hold.

**Status surface:** report "redundancy degraded" for existing data during every window, separately from the class of
new writes. Whether that is also reported per answer waits for AFR-D-14.

## 4. AFR-D-02: durability boundary (end-of-log rule)

**Recommendation: keep on HOLD** (Codex agrees). The models assume a durable log and choose nothing here.

**Already fixed by D-01 and D-03, if accepted:**
- positions are (term, LSN), with a durable term history (G-15, R6);
- with replicas, the new primary decides the end of the log, and I4 preserves acknowledged writes.

**The open choice, for any node or window with one copy** (G-14(b), CD section 4):
- (a) a durable per-flush watermark;
- (b) self-describing flush frames with a per-segment nonce;
- (c) both.

(b) alone cannot tell a newest flush that was silently lost from an unwritten tail. CD leans to (c) for replicated
groups, and to G-14(a) only for a node without replicas, unless the watermark is cheap enough.

**X-2, to preregister** (decision 5 authorizes the preregistration, not a run):
- **What:** the dsync write cost of a per-flush watermark and of a per-segment nonce, on the R640 data disk, with
  the frozen manifest.
- **Performance criterion:** cody sets the bound before any run. SA proposes 10% for the watermark to become the
  single-node default.
- **Correctness criterion:** an injected fault in the newest flush (zeroed, torn, silently dropped) is detected and
  refused, or is provably beyond the acknowledged boundary. It is never silently accepted.

**Single-node recovery (Codex obligation 1). Options, not yet contract text.** A sole copy that restarts (a node
without replicas, or the primary of a degraded group) must resume without reusing a (term, LSN). Its term cannot simply change on restart, because G-55 says a restart does
not change the HA term (HOLDS).
- **Option (i), recommended: never reuse the LSN.** The node durably reserves LSNs in blocks before using them, the
  same pattern as today's mint lease blocks [FACT-SNC: E11]. After a restart it resumes above the reserved block, so
  a discarded tail's LSNs are skipped, not reused. The term does not change.
  - Cost: the WAL format must allow gaps in LSNs (AFR-D-06).
- **Option (ii): a local term bump on restart.** This needs G-55 amended for nodes without replicas, a durable place
  for the term, and a reseed or a term floor above the node's local maximum when it later joins a group. Otherwise
  the group's future terms collide with the node's own.
- **Limits common to both:**
  - if the write that persisted the reservation or the term is silently lost, nothing local detects it (CD section
    4, "superblock newest write lost");
  - a superblock fallback to its older copy (AFR-R-23) must refuse, since there are no witnesses to check against.

  Both limits are the G-14(b) boundary problem, so they wait on the D-02 choice.

## 5. What still blocks designing the Rust architecture safely

None of these blocks decisions 1-3. Each must be settled before the crate it names is designed (SG section 14 lists
all decisions "required before Rust crate design").

| Item | Blocks | Depends on |
|---|---|---|
| AFR-D-02, X-2 and single-node recovery | WAL and recovery crate | section 4 |
| AFR-D-06 format and protocol versioning (term in every record, LSN gaps) | every on-disk and wire format | — |
| AFR-D-07 trust model; forged lease frames (AFR-R-21) | network and replication crates | — |
| AFR-D-13 consensus service: durable witness state (F9), linearizable reads (A9), lagging witnesses (A7) | control-plane crate | D-03 |
| AFR-D-11 membership change | control-plane crate | D-03 |
| AFR-D-10 default read consistency | client API | D-01 |
| AFR-D-14 status and IN-DOUBT taxonomy | client protocol | — |
| AFR-D-15 refusing weak configurations | configuration | may fold into D-03 |
| AFR-D-04 ordering, D-05 sessions, D-08 sessions across migration, D-09 schema evolution, D-12 slot hash | their own crates | independent of this package |
| G-37 migration: seal **and** expiry of any older-term source lease (CD 5.2), not modelled | migration design | D-01 |
| R9 model coverage (Codex obligation 2) | standby reads, if they are in the first Rust scope | — |
| G-13 host-crash durability (UNPROVEN) | durability claims, not the design | evidence |
| Lease and timeout lengths (X-3) | tuning only | a prototype |

## 6. Codex's remaining obligations (PR #147 review, section 3)

| # | Obligation | Status |
|---|---|---|
| 1 | Standalone node: new term after a torn tail, without reusing a (term, LSN) | Options in section 4. Not contract text. The QA widened it to any restarted sole copy, including a degraded group (section 7) |
| 2 | Targeted model coverage for R9 (degrade and acknowledgement) | Not started. Needs standby leases in `Combined.tla`, or writes in `Authority.tla`. Small, but new modelling, so it waits for a go |
| 3 | X-2 with preregistered criteria | Not started (decision 5) |
| 4 | Keep A2, A7, A8 and A9 with their limits | Kept as in AR section 4. Nothing promoted |
| 5 | Independent QA of PR #147's corrected text | Done: 6 major and 5 minor findings; the text was fixed in this PR, and two items stay open (section 7). The fixed text has not been re-reviewed |

## 7. Independent QA of PR #147's corrected text

An independent adversarial QA re-reviewed the text merged in PR #147 (`747bfcf`). It found no blocker, but **six
major and five minor findings**. Its conclusion: the clauses should not be frozen yet. It did not rerun TLC.

**Fixed in this PR:**
- **G-14(a) and the term history.** SA allowed an automatic rebuild of a damaged term history, and G-14(a) forbade
  it. Now any damage refuses, and a rebuild or reseed is an operator action. H needs a checksum and a durable
  compaction floor, so that damage cannot pass for compaction.
- **G-14(a) and torn tails.** (a) now excepts a torn tail that (b) permits.
- **G-15 condition (1)** is now triggered by any restart, as R6.0 is.
- **G-15 condition (3)** forbids removing any content acknowledged at LOCAL_DURABLE or above, unless it is still in
  the primary's log. The exceptions are weaker configurations, R7 outside S, and the 5c override.
- **G-15 "Open".** The note now covers every restarted sole copy: a node without replicas, or a degraded group with
  S = {p}. R1-R9 give that copy no way to resume, which is also a G-48 gap.
- **R9.**
  - "May still be valid" is defined as `W = d·(1+ρ)` after p's last grant.
  - A leased standby applies an entry before acknowledging it [INF].
  - G-30's evidence lists three C++ gaps: no `(1+ρ)` factor, the lease record is lost on restart, and the clock is
    CLOCK_MONOTONIC.
- **G-46** names the exact overlap condition. The fault table now lists VM pause as out of contract.
- **G-48** covers every G-14(a) refusal. The header revision line now names G-48.
- **RESULTS section 8 and the README** say that `HistoryUnique` exercises R6.0 only. The model has no torn tails
  and no volatile tail on a standby.

**Still open:**
- **The restarted sole copy.** This joins Codex obligation 1 (options in section 4). Option (i) may also cover the
  degraded group. That has not been analysed.
- **R9's new clauses are not modelled** (Codex obligation 2).

## 8. Corrections from the QA of this package

| Finding | Correction |
|---|---|
| The single-node proposal contradicted G-55 and called itself "the same rule as R6.0" | Replaced by options (i) and (ii); G-55 is named; collision on joining a group is stated |
| The `m = 1` column relied on a G-11 fix that has not been made | Made a condition of decision 3 |
| The lease was described as coming from S, not A; CD 5.1 conflicts with SA | Corrected; SA is stated to supersede CD 5.1 |
| It contradicted CD 6 without saying so | Decoupling explained; the term-in-every-record consequence named |
| The `m` table understated the stalls | Reads, the barrier, A, and R9's wait added to each row |
| Kafka was used as a match | Kept as a reference point, with the R2 difference stated |
| R9 coverage misstated | Split into the checked clause and the unmodelled G-30 clause |
| Decision 2 asked cody to accept text that does not exist | The E1-E11 rewrite and the SG section 14 update are made conditions |
| Section 5 was incomplete | D-04, D-05, D-08 to D-12, G-37 and G-13 added |
| D-01's argued evidence was thin | R4 durability, READY UNRESOLVED and the missing combined run listed |
| Minor: C++ overclaim, circular second defence, "Done" with a placeholder, bundled ask, wrong cite | All corrected. X-2 preregistration is now its own decision |
