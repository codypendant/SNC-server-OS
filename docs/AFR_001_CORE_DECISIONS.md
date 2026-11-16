# SNC-AFR-001: three core decisions, evaluated together (for owner review)

- **Status: PROPOSED.** This document decides nothing. Each recommendation becomes binding only when cody accepts
  it, after Codex has reviewed it. It authorizes no code, no C++ slice and no Rust work.
- **Scope.** The three decisions are evaluated together, because each one constrains the other two (Codex, PR #139
  final review):
  - AFR-D-01, the authority model;
  - AFR-D-02, the durability boundary (end of log);
  - AFR-D-03, the replication-group model.
- **Baseline.** `main` at `15d3cf4`. Every file:line citation refers to that commit.
- **Inputs.**
  - `docs/SNC_GUARANTEES.md`, section 14.1 (what D-01 must specify) and section 14.2 (what D-03 must specify);
  - the Phase 1 notes `docs/afr-001-working-notes/A_core.md` (sections 5 and 6) and `B_distributed.md` (sections 2
    to 6);
  - CORE_CONTRACTS.
- **Evidence labels.**
  - VERIFIED: read in code at the cited line.
  - TESTED: covered by a named test or experiment.
  - INFERRED: reasoned from code; not run.
  - UNMEASURED: a cost or performance claim with no measurement behind it yet.

## 1. Summary for cody

| Decision | Recommendation | Main reason | Must be measured before acceptance |
|---|---|---|---|
| **D-03 group model** | **Generalize today's pair into an in-sync set.** Witness consensus decides each group's term and its in-sync member set. Every write is acknowledged by every member of that set. Removing a member (today's "degrade") and promoting a member are both consensus decisions, so they exclude each other. Do **not** switch to majority-acknowledged replicas. | It is the model the C++ already implements for pairs, and its safety pieces are TESTED (EL2 to EL4). One failure is tolerated with two data copies, where a majority quorum needs three. It folds the N-member placement map and the pair-only HA state into one source of truth. | Write latency against the slowest member; time to remove a slow member |
| **D-01 authority** | **Term-scoped leader leases granted by the in-sync set**, renewed on the existing acknowledgement traffic. Durable fencing and a restart that re-acquires authority before serving. Quorum-confirmed reads are offered as an opt-in. | It closes the stale-read, O1 and non-durable-fencing gaps with one mechanism and keeps the read path local (INV 14). It mirrors the standby read lease the C++ already has (VERIFIED). | Lease renewal cost; failover delay added by waiting out a lease |
| **D-02 durability boundary** | **Log positions become (term, LSN). Flushes are self-describing. In a replicated group the boundary is reconciled with the in-sync set before the node serves.** A node with no replica gets the detection guarantee only (G-14(a)), unless the owner chooses a per-flush watermark after it is measured. | Detection alone cannot see a lost newest flush. Only a second durable copy can (section 4). The in-sync set already holds that copy, because records ship before the local write (VERIFIED, `storage.cpp:651`). | Cost of the optional watermark on the R640 data disk |

The three recommendations stand or fall together:
- The in-sync set is who grants leases (D-01).
- It is also where the acknowledgement boundary lives (D-02).
- The term that fences authority is the same term that makes log positions unique (D-02).

Section 6 lists these couplings.

## 2. Facts that bind all three decisions

| # | Fact | Evidence |
|---|---|---|
| F-1 | The HA unit is a pair. Placement allows N members per group, but HA state does not know them. | VERIFIED: `snc_witness.cpp:531-534`, `placement.h:44-48` |
| F-2 | A sync write is acknowledged only when it is locally durable **and** acknowledged by the standby. | VERIFIED: `shard.h:555-559` |
| F-3 | The "degrade" grant (continue without the standby) and a promotion are decisions on the same consensus value, so they exclude each other. | VERIFIED: `snc_witness.cpp:852-870`; TESTED in `test_witness.py` |
| F-4 | Each flush is shipped to the standby **before** the local write is issued. | VERIFIED: `storage.cpp:651` |
| F-5 | A primary may not degrade while a read lease it granted to its standby is live. That lease is renewed on acknowledgement traffic and assumes rate drift below 10%. | VERIFIED: `shard.cpp:2254-2256`, `replication.cpp:517-522` |
| F-6 | The primary holds no lease. Withdrawal is an in-memory flag, and a restart serves at the old term. | VERIFIED: `engine.cpp:1968-1970`; TESTED in QA 2 N6 |
| F-7 | Recovery cannot tell a zeroed or lost newest flush from unwritten space. A zero block ends the scan, and segments are preallocated with zeros. | VERIFIED: `storage.cpp:393-395`, `:554-555` |
| F-8 | After truncation the stream resumes at `last+1`, so LSNs are reused. The standby may still hold the truncated records (F-4). | VERIFIED: `engine.cpp:1188`; TESTED in QA 1 |
| F-9 | The superblock falls back silently to its older copy. Term, mint leases and the active checkpoint live there. | VERIFIED: `storage.cpp:210-226` |
| F-10 | Membership is static. Witnesses, pairs and placement are each configured separately. | VERIFIED: `B_distributed.md` 6 |

## 3. AFR-D-03: the replication-group model

Section 14.2 of the spec asks for both models to be specified on the same six points.

| Point | (A) In-sync set with external consensus (generalized pair) | (B) Majority-acknowledged replicas (Raft-like) |
|---|---|---|
| Quorum intersection | A write is acknowledged by **all** members of the current set. Promotion picks a member of the current set. Shrinking the set and promoting are consensus decisions on one value (F-3). So every promotable member holds every acknowledged write. | A write is acknowledged by a majority, and election needs a majority vote. Any two majorities intersect, so the elected leader holds every committed write. |
| Data commit | Committed when every set member has it durably. A member may hold records that were never acknowledged (F-4). Those are in-doubt (G-58) and are reconciled at the next term. | Committed when a majority has it. Minority members may hold uncommitted suffixes, which the leader truncates. |
| Witness role | Witnesses decide term, set membership and placement. They hold no data. Three or five small processes serve many groups. | No witness is needed for data. Election runs among the replicas. Placement still needs a consensus service. |
| Promotion eligibility | Only a member of the current in-sync set (E1 to E11 restated). A member outside the set, including an async member, is never promoted automatically. This closes R-10 by construction. | Only a replica with the most up-to-date log can win an election. |
| Membership change | Each change is one consensus decision on the group record. New members catch up outside the set and join when caught up, as the C++ readiness proof already does (`B_distributed.md` 2.2). | Needs joint consensus or single-server changes, which are a well-known source of subtle bugs. |
| Failures tolerated | f failures with f+1 data copies, provided the witnesses are available to shrink the set. | f failures with 2f+1 data copies. No external dependency. |
| Write latency | The slowest member of the set. A slow member stalls writes until it is removed (degrade timeout). UNMEASURED beyond the existing sync figure (about 60% of async throughput, VM). | The median member. A single slow replica does not stall writes. |
| C++ continuity | High. It is the existing pair model with the pair widened to a set, and the safety pieces are TESTED (F-2, F-3). | Low. It needs a new log-replication and election protocol. The C++ reference could not serve as a behavioral oracle for it. |

**Recommendation: (A).**

- It is the model whose safety evidence already exists, and it needs a third of the data copies for the same
  tolerance.
- The C++ reference stays a valid oracle for it.
- Its weaknesses are known and bounded:
  - write latency follows the slowest member;
  - removing a member needs the witnesses.
  Both already appear as F6 and F7 and are liveness issues, not safety issues.
- What (A) must add to the C++:
  - one group record holding the term, the set and the mode, derived from placement (removes F-1 and F-10);
  - consensus-driven membership change;
  - async members that are never in the set.

**Revisit (B) only if** measurement shows that tail latency against the slowest member breaks the latency criteria
the validation campaign sets (CRITERIA.md is not yet written; `docs/EXPERIMENTAL_VALIDATION.md`).

## 4. AFR-D-02: the durability boundary

G-14(b) requires recovery to restore every acknowledged write or refuse. These are the cases detection alone
cannot catch:

| Case | Detectable by framing and checksums alone? | What can detect or repair it |
|---|---|---|
| Damage inside a flush (bit flip, torn record) | Yes | Fail closed (the AFR-S-1 slice) |
| Unknown record kind | Yes | Fail closed (AFR-S-1) |
| Newest flush zeroed | Only if unwritten space is distinguishable from zeros, for example a per-segment nonce written when the segment is formatted | Formatting with a nonce. Every flush header carries it |
| Newest flush silently lost (the device acknowledged a write it never made) | **No.** The space looks unwritten | A second durable copy: a replica (in-sync set member) or a per-flush watermark written elsewhere |
| Superblock rolled back to its older copy (F-9) | **No.** The older copy is valid | Cross-check term, lease block and epoch with the witnesses before serving. The witnesses hold the newer values |
| LSN reuse after truncation (F-8) | — | Make log positions (term, LSN). A new term after recovery means a truncated suffix can never be confused with new records, and a rejoining member truncates its divergent suffix instead of moving the directory aside |

**Recommendation.**

1. **Positions are (term, LSN).** This closes LSN reuse (R-03) and gives replica reconciliation a precise rule.
   **Open:** whether recovery bumps the term (G-55 says a restart does not today) or a separate durable incarnation
   is used. Proposed: an incarnation, so restarts do not touch the HA term.
2. **Self-describing flushes.** Each flush records its LSN range, the segment nonce and a checksum, and segments are
   formatted with that nonce. This detects zeroed and torn flushes. It is a format change, so it waits for AFR-D-06.
3. **In a replicated group, the boundary is reconciled with the in-sync set before serving.**
   - A recovering member serves nothing until it has compared its end of log with the set.
   - The set's copy wins for acknowledged records.
   - This is the same barrier D-01 needs on restart (section 5).
   - It is what makes G-14(b) achievable with no extra synchronous write.
4. **A node with no replica gets G-14(a) only.** A silently lost newest flush is out of contract and documented.
   The owner may instead choose a per-flush watermark: a second synchronous write before each acknowledgement, to a
   separate location. Its cost is UNMEASURED, so measure it first: experiment X-2 in section 6.
5. **Check the superblock against consensus.** At start, the superblock's term, lease block and epoch are checked
   against the witnesses. The start barrier already reads placement linearizably for a directory that was last a standby,
   malformed or legacy (`engine.cpp:999-1020`, `B_distributed.md` 2.1), but it checks no term or lease.

Points 1 and 2 are format decisions and wait for AFR-D-06. The minimal fail-closed C++ slice (AFR-S-1) needs none
of them.

## 5. AFR-D-01: the authority model

These are the safety invariants every option is judged against (spec section 14.1):
- G-28: read your acknowledged writes;
- G-31: one serving owner;
- G-32: a newer term is durable and final;
- G-34: status follows authority;
- G-37: the migration serving proof.

### 5.1 The options on each path

| Path | (L) Leader lease granted by the in-sync set | (Q) Quorum-confirmed authority |
|---|---|---|
| Primary write | Unchanged: the in-sync set acknowledges (F-2), and a fenced member refuses an older term. | Unchanged. |
| Primary read | Answered locally while the lease is valid. | Answered after a confirmation round with the set (batched per tick). One round trip added to read latency. |
| Durable fencing | The highest known term is persisted before the node answers anything (needed by both options; fixes R-12). | Same. |
| Promotion | The new primary waits until every lease of the old term has expired. The witnesses know the lease bound. | No wait. The old primary's confirmations fail as soon as the set member is fenced. |
| Restart | The node serves nothing until it holds a fresh lease, and its log end has been reconciled (D-02 point 3). | The node serves nothing until it is confirmed and reconciled. |
| Migration | The destination activates only after the source group's lease for the slot has expired, or after a seal by the current lease holder. This closes the unreachable-source window (`B_distributed.md` 5.2). | The source cannot confirm once it is fenced, but an unreachable old source that is not yet fenced can still confirm with its own set. The commit gate must fence the source term first. |
| Partition recovery | Expired leases make the cut-off side refuse. When the cut heals, the node learns the newer term durably. | The cut-off side cannot confirm, so it refuses at once. |
| Degraded mode (set shrunk to the primary alone) | Lease granted by the witnesses, renewed by consensus reads (expensive, but rare). | Confirmation through the witnesses on every read batch: a Paxos round per batch. |
| Status (G-34) | Reports `primary` only while the lease is valid. | Reports `primary` only after the last confirmation, within a stated age. |
| Clock assumption | A stated maximum rate drift between grantor and holder, the same assumption the standby lease already makes (F-5). Safety must fail closed when the bound is exceeded: the holder uses a lease shortened by the bound. | None. |

### 5.2 Recommendation: (L), with (Q) as a per-request opt-in

- SNC's thesis is that operations run on the owning core with no network round trip (INV 14). Under (Q) every
  default read would pay a round trip.
- (L) reuses machinery that already exists and is reviewed: the standby lease, renewed on acknowledgement traffic
  (F-5). The new part is the same lease in the other direction.
- (L) also fixes the migration window, which (Q) does not fix without an extra fencing step.
- Its cost is real:
  - a failover waits out the lease (UNMEASURED; bounded by the lease length);
  - the drift assumption, which the standby lease already makes.
- Clients that cannot accept a clock assumption choose (Q) per request.

**Stale-authority rejection, on every path.** A node that sees a newer term, or an expired lease, answers with a
refusal that names the newer term (or "authority unproven"). It persists the newer term before answering anything
else, and it reports a non-primary status. This is the same for both options.

## 6. Couplings, and what to measure

The couplings:
1. Under D-03 (A), the in-sync set grants D-01's leases. In degraded mode the witnesses grant them.
2. D-02's boundary reconciliation and D-01's restart rule are one barrier: reconcile, take the lease, then serve.
3. The term that fences authority (D-01) is the term in D-02's log positions.
4. The superblock cross-check (D-02 point 5) needs consensus-held term and lease values, which D-03 (A) keeps in
   the witnesses.
5. Under D-03 (A), async members are outside the in-sync set. They never grant leases and are never promoted
   automatically, which closes R-10.

Choosing (B) for D-03 would change couplings 1, 4 and 5. Leases would come from a replica majority, and the
superblock cross-check would go to the replicas. Accepting the three decisions one at a time is therefore not
recommended.

| Experiment | Measures | Decides |
|---|---|---|
| X-1 | Write latency (p50, p99) with an in-sync set of 2 and of 3, each with one slow member, on the R640 | D-03 (A) versus (B) |
| X-2 | Throughput and latency with a per-flush watermark, against today, on the R640 data disk | D-02 point 4 for nodes with no replica |
| X-3 | Lease renewal overhead on acknowledgement traffic, and the failover delay added by lease expiry | D-01 (L) |

Each experiment needs its own preregistration and authorization, per AGENTS.md change discipline. None is
started.

## 7. What accepting these recommendations would freeze, and what it would not

**Frozen on acceptance:**
- the group model: an in-sync set under witness consensus;
- the authority mechanism: term-scoped leases granted by the set;
- the reconciliation barrier on restart;
- (term or incarnation, LSN) as the log identity.

**Not frozen:**
- byte formats and versions (AFR-D-06);
- the trust model (AFR-D-07);
- lease lengths and timeouts (measured, X-3);
- the watermark option (X-2);
- membership-change mechanics beyond "one consensus decision per change" (AFR-D-11).

Acceptance would not prove the C++ reference satisfies anything, and it would not authorize Rust (spec section 16).
