# SNC-AFR-001: safety analysis of the proposed authority, group and durability protocol

- **Status: PROPOSED analysis, not a proof.** This is Revision 2.
  - Codex's review of PR #140 (`docs/handoff/PR140_141_Codex_Review.md`) asked for this analysis before final
    architectural acceptance. Codex's review of PR #142 (`docs/handoff/PR142_Safety_Analysis_Codex_Review.md`)
    required four changes, which section 7 lists.
  - Choosing a direction (`docs/AFR_001_CORE_DECISIONS.md`; cody chose leases for D-01) is separate from proving the
    protocol. This document is a hand argument toward the second.
  - Every step is an inference unless it is labelled otherwise. Section 6 says what would turn the argument into
    evidence.
- **Closure phase.** `docs/AFR_001_VERIFICATION_CLOSURE.md` follows Codex's Revision 2 review: case 5c is now
  forbidden by R7, R3's simplified rule and a combined authority-and-durability model are model-checked within
  bounds, and a defect in the Revision 2 Log model is disclosed.
- **Owner decisions, 2026-11-16** (`docs/handoff/PR148_Owner_Decisions.md`): the authority mechanism here (R3, R4, R5,
  R8, the lease from A) is conditionally accepted as AFR-D-01. The in-sync-set direction is selected for AFR-D-03, but
  R2 is not frozen (`docs/AFR_001_R2_AND_SINGLE_COPY.md`). R9 was revised after its targeted model (R9-A to R9-E, 2026-11-17). **Owner decision 2026-11-17** (`docs/handoff/PR155_Owner_Decision.md`): R2′ and R9-A to R9-E are conditionally frozen, with the exclusions and conditions stated there; R9-F is not. This is not a proof.
- **Nothing here authorizes implementation.** AFR-D-02 is on HOLD. R1, R2, R6 and R7 are not accepted as a complete
  protocol; they are the D-03 direction's current candidates.
- **Method.** This follows `docs/ENGINEERING_PRINCIPLES.md` (research before reinvention). Each claim carries a
  label:
  - **[FACT-SNC]**: verified in the code at `5338688`.
  - **[EXT]**: a guarantee another system documents, cited from its primary source.
  - **[INF]**: an SNC inference made here.
  - **[HYP]**: unverified.
- **Review.** An independent adversarial reviewer walked every hand-off case as a concrete interleaving:
  - the first draft failed both claims (B1 and B2), and three further defects were found (M1, M2, M4);
  - Codex then found four more gaps;
  - the rules below include every fix;
  - cases with no rule that prevents them are marked **UNRESOLVED**.

## 0. Protocol under analysis

### Timing

- **Clock.** Each node's clock counts through suspend (CLOCK_BOOTTIME) and runs at a real rate in `[1-ρ, 1+ρ]`.
- **Holder side.** A holder treats a grant as valid for `d·(1-ρ)` on its own clock, measured from when it **sent**
  the request. That lasts at most `d` real seconds, starting no later than the grant.
- **Waiter side.** A waiter waits `W = d·(1+ρ)` on its own clock, which lasts at least `d` real seconds.
- **Consequence.** A wait of W that starts at or after a grant ends after every lease that grant supports.

Revision 1 wrote the wait as `d·(1+ρ)/(1-ρ)`. That was stricter than needed and did not match the drift condition in
section 1. The correct value is W above. [INF]

### Group record R

R is held by the witnesses and changed only by compare-and-set on `version`, decided by the witnesses' Paxos. It
holds:
- the term `t`;
- the primary `p`;
- the in-sync set `S` (p is in S);
- the mode, normal or degraded (degraded means S = {p});
- the minimum set size `m`;
- `ready`.

It generalizes today's `ha/G` state (`snc_witness.cpp:112-138`, [FACT-SNC]). It follows PacificA's versioned
configuration, where the first accepted change wins [EXT: PacificA, MSR-TR-2008-25, section 2.2].

**Linearization point of a change to R:** the instant a majority of witnesses has accepted the new version, that is,
when it is chosen. No node acts on a new version before it learns that the version was chosen.

### Member state

Each member `i` keeps:
- a durable highest term `T_i`;
- a log of entries `(term, lsn, payload)`, where each WAL record carries its term (a D-06 format item);
- a term history `H_i`, mapping each term to its first LSN in `i`'s log. It plays the role of Kafka's leader-epoch
  sequence [EXT: KIP-101], but it is derived from the log (below).

**[RECORDED REVISION, CONDITIONALLY APPROVED: owner decision 2026-11-18, `docs/handoff/PR157_Owner_Decision.md`;
final acceptance depends on AFR-D-02.]** `H_i` is derived, not stored separately. It is the sequence of terms on
`i`'s retained, validated, non-void WAL records (each record carries its term), above a durable compaction floor. The
truncation intent in the node metadata record bounds it (`docs/AFR_001_D06_STORAGE_METADATA.md` 4.3). The floor of
each stream is the CRC-protected `retain_from` in the active checkpoint manifest. The floor term is the `prev_term`
field in the header of the segment that starts there (D-06 4.2).

The owner's conditions, verbatim from Codex's review:
1. "The term history must be reconstructible from validated retained WAL records and a durable compaction floor."
2. "WAL corruption must never silently become an acceptable history truncation."
3. "The revised contract must explicitly preserve G-14(a)'s refusal behavior."
4. "Final acceptance depends on D-02's durability-boundary design."

Realized as:
- **Refusal (conditions 2 and 3).** The member refuses with a named error when:
  - no segment starts at `retain_from`, or a segment is missing above it;
  - two segments leave a gap between them;
  - a record that cannot be validated lies below the newest segment's valid end.

  Damage inside an older segment is caught this way, because it leaves a gap before the next segment's first LSN.
  Rebuilding or reseeding is an explicit operator action (R6.4), as G-14(a) requires.
- **Torn tail.** A torn tail in the newest segment is discarded only as G-14(b) permits.
- **Not yet met (condition 4).** Condition 2 is **not met** for the newest segment until D-02 closes it. Telling a
  torn or lost newest flush, a lost newest segment, or a lost truncation segment `L+1` (which would revive a void tail)
  from the log's real end is D-02's. So is metadata rollback (R-23). Until then, such damage can silently shorten
  `H_i`.

A member is reseeded (R6.4) when any of these holds:
- its end or its truncation point is below the answering member's floor;
- its last term is below the answering member's floor term.

[Earlier text, frozen 2026-11-17: "`H_i` must be durable, with a checksum and a durable compaction floor, so that damage
is distinguishable from compaction. If it is damaged, the member refuses with a named error (G-14(a)); rebuilding it
from the log's term stamps or reseeding is an explicit operator action (R6.4)." Revised after Codex's PR #146 review;
previously: "If it is lost, the member is reseeded."]

### Ack set A and lease

- **Acknowledgement.** The primary acknowledges entry `e` only when two conditions hold:
  - every member of its ack set `A` has `e` durably (p included);
  - p holds valid authority at that moment.
- **Normal-mode authority.** It is the minimum over grants from every member of A other than p. A member that has not
  granted counts as an expired grant. [EXT: PacificA section 2.3; TiKV anchors the lease at proposal time,
  https://tikv.org/blog/lease-read/]
- **Degraded-mode authority.** See R8.
- **Amendment to CORE_DECISIONS 5.1 (noted, not yet applied).** That document takes the lease from every member of S.
  It must be every member of A.

### Rules

**R1. Ack set ordering.**
1. To add `c`, p puts `c` in A first, waits until `c` has caught up (defined in section 2), then proposes the
   compare-and-set adding `c` to S.
2. p may drop a member from A only after one of two things:
   - a shrink compare-and-set that removes it has been chosen; or
   - a linearizable read of R, **still at p's own term**, shows it is not in S. If the read shows a higher term,
     p steps down (R4) and drops nothing.

   A lost reply to a grow is therefore never treated as a failure (fixes B1). R9-C adds a second condition: no
   removal while a standby lease p granted to that member may still be valid. The "still at p's own term" condition
   was added after the TLA+ model found a counterexample without it (`spec/tla/RESULTS.md`, finding T2). In that
   counterexample, a replaced primary that had not yet learned of its replacement read that a member had left S,
   dropped it, and acknowledged a write alone.

[EXT: Raft thesis 4.2.1 and 4.3; PacificA section 2.4.]

**R2. Promotion and READY.**
- **Automatic promotion** is a compare-and-set to `t+1` naming a new primary `j`. Four conditions apply:
  - `j` is in S at the chosen version;
  - `ready` is true;
  - `j` is in sync (E1-E11);
  - the new S is `{j}`, which is today's behavior (`snc_witness.cpp:871`, [FACT-SNC]).
- **READY at `t'`** may be recorded only after the primary of `t'` has passed its R3 eligibility point. This fixes B2.

**R3. Eligibility to serve (fixes Codex issue 1).**
- **Grant bookkeeping.** Each member keeps `last_grant_i` in memory. It is the clock time of its most recent grant to
  any primary, and it is updated **before** the grant reply is sent.
- **Stop granting.** A member stops granting to terms below `t'` the moment it persists `t'` (R4).
- **When it may serve.** A member promoted to `t'` may serve only once its own clock reaches `learned_i + W`.
  `learned_i` is when it persisted `t'` after learning that its promotion was chosen.
- **Why this one bound covers every earlier grant** [INF; model-checked within bounds in the closure phase through
  `R3Lemma`, see `docs/AFR_001_VERIFICATION_CLOSURE.md` section 3]:
  - A member grants only to its own known term, and stops granting to lower terms when it persists `t'` (R4).
    So every grant it made to a lower term, **including grants made after a restart or during any wait**, was made
    at or before `learned_i`.
  - A restarted primary never serves its old term again (R6.0), so `learned_i` always comes after its last boot.
  - `boot_i + W` and `last_grant_i + W` are therefore always ≤ `learned_i + W`.
  - Revision 2's draft listed all three bounds. The independent QA showed the first two are implied by the third,
    so only the third is kept.
- **The same bound fences witness grants** (R8) and standby leases (R9).
- **What serving means.** Acknowledging writes, answering reads, granting standby leases and recording READY.
- **Interleaving the old rule allowed** (d = 2, ρ = 0.1, W = 2.2):
  1. j restarts at 0.00.
  2. p sends a renewal at 1.00, and j grants at 1.01. The old rule allowed granting at once.
  3. p's lease runs to 2.80.
  4. The old restart-only wait ends at 2.20. j is promoted, learns it at 2.30, and serves until 2.80: two holders
     for 0.5 s.
- **Under R3:** j learns at 2.30 and becomes eligible at 4.50, after p's lease has ended. j's grant at 1.01 came
  before it learned, as every grant to a lower term must.

**R4. Durable fencing.** A member that learns `t' > T_i` persists `t'` before anything else. From then on:
- it refuses records, grants and acknowledgements for lower terms;
- as a holder, it stops serving.

**R5. Answer-time check.** For a read, p captures the state, then checks its authority, then sends.

**R6. Reconciliation (fixes Codex issue 4; the invariants are in section 2).**
- **R6.0 Term ownership.** Entries of term `t` are created only by the primary chosen for `t`.
  - A primary that restarts never creates more entries under its old term. It serves again only through a new
    promotion.
  - Why this matters: `storage.cpp:651-652` ships a batch to standbys before the local write [FACT-SNC]. A restarted
    primary may have lost a tail that a standby holds, and reusing `(t, lsn)` for different content would break
    log matching.
  - KIP-101 has the same requirement: a leader that restarts without an election still gets a new epoch [EXT].
- **R6.1 Drain first.** A new primary `j` writes no entry of `t'` until every older-term entry it received is
  durable.
- **R6.2 Finding the divergence point.** A rejoining member `k` finds it by iteration [EXT: KIP-279, verified
  against the original page, section 7]:
  1. `k` sends its last term `t_k`.
  2. `j` answers with `t*`, the largest term ≤ `t_k` present in `H_j`, and `end_j(t*)`, the last LSN in `j`'s log
     whose term is ≤ `t*`. `j`'s log here means the log `j` ships from, including entries not yet durable on `j`.
     Under R6.0, if `j` loses them it stops being primary.
  3. `k` truncates to `min(end_j(t*), end_k(t*))`.
  4. `k` repeats until its last term is in `H_j` and its end LSN is ≤ `end_j` of that term.
- **R6.3 Durable truncation.** Before truncating, `k` durably writes an intent `(t_j, L)`:
  - recovery re-applies a pending intent before anything else;
  - `k` clears the intent durably before it accepts any entry beyond `L`;
  - `k` takes no checkpoint while an intent is pending.

  - **R6.3′ (proposed 2026-11-17; full text in `docs/AFR_001_PROMOTION_DURING_RECON.md` 2.1).**
    1. A reply is bound to its request (group and stream, boot incarnation, request number, `k`'s last term and
       durable end LSN) and carries
       the answering primary's term. `k` uses it only if the term equals its durable term, the binding matches its
       latest request and an unchanged log, `k` is not primary, not positioned, not incomplete, has no pending intent,
       and the truncation point is not below its current or in-progress checkpoint (R6.4 re-checked).
    2. The check and the intent write are one step, serialized against R4's term write, positioning, checkpoint
       publication and S2′.
    3. While an intent is pending, `k` sends no request, is not positioned, counts toward no acknowledgement or READY,
       holds no lease and takes no checkpoint, in any term. On learning a newer term it persists the term first (R4),
       then completes the intent, in-process, before any other action in that term.
    4. A promoted node completes any pending intent before it serves (S2′). Completion is a crash-consistent
       procedure, including a state rebuild (2.2 there).

  The checkpoint rule was added after the TLA+ model found a counterexample without it (finding T1). A checkpoint taken
  after the intent but before the truncation records state that the truncation then removes from the log, and the
  R6.4 check, made before the checkpoint, does not see it.
- **R6.4 Reseed.** `k` is reseeded in place of R6.2-R6.3 in either of two cases:
  - the truncation point is below `k`'s checkpoint maximum LSN (checkpoints are local-only, `shard.cpp:2710-2716`,
    [FACT-SNC]);
  - `H_j` cannot answer because it no longer covers `t_k` (compacted). A damaged `H_j` (G-14(a)) is not this case: `j` refuses
    like any member.

  If `H_k` is damaged (G-14(a); Member state, "Refusal"), `k` refuses with a named error instead (G-14(a)).
  Reseeding it then is an explicit operator action. [Revised after Codex's PR #146 review; wording follows the
  `H_i` revision of 2026-11-18.]
- **R6.5 Incomplete.** A durable `incomplete` flag is set before `k` discards anything, and cleared only once the
  copied state is durable and positioned at a `(term, lsn)` of `j`'s log.
  - While the flag is set, `k` acknowledges no entries, is not counted in A, cannot be added to S, and cannot be
    promoted.
  - Recovery with the flag set restarts the reseed.
- **R6.6 No silent fallback.** Kafka falls back to the high watermark when the epoch query fails (KIP-279 [EXT]). SNC
  does not: a failed or unanswerable query leads to a retry or to R6.4, except that a damaged `H_k` leads to a
  refusal (R6.4).

**R7. Operator promotion.**
- It carries the expected term and goes through the same compare-and-set, so R3, including the `learned + W`
  barrier, applies.
- It may name a member outside S only in one of two cases:
  - the record is degraded **and READY at its current term**, meaning the degraded primary has taken over (R2);
  - every member of S is **confirmed** fenced: a fencing action (for example power-off) has reported that each one has
    stopped (STONITH; [EXT: PostgreSQL warm-standby failover docs],
    [EXT: Pacemaker fencing]). An operator's belief that a member is down is not a confirmation.
  - **Boot read.** A fenced member may come back (Pacemaker's default fencing action is a reboot). When any member
    boots, it reads R from the witnesses and adopts its term before it grants. Without this, a returning member that
    never learned the new term renews the old primary's lease, and 5c reappears (found by the closure QA; TLC
    `U_noBootRead`).
- **Closure (case 5c).** Any other outside-S promotion while `|S| ≥ 2` is not a protocol operation. If SNC offers
  it at all, it is an explicitly unsafe recovery override, labelled as able to break Claim L and lose acknowledged
  writes, outside the contract. This matches how other systems treat the same step (see
  `docs/AFR_001_VERIFICATION_CLOSURE.md` section 2).
- **Why READY is needed.** The independent QA found this interleaving, and the model reproduces it
  (`op_outside_degraded_unfenced`):
  1. Term 2 is chosen for `j` (degraded, S = {j}), but `j` has not learned it.
  2. The operator promotes `k`, outside S, to term 3.
  3. `j` still grants to the term-1 primary, which stays alive.
  4. W1 and the barrier fence only the term-2 holder, so the term-1 primary and `k` overlap.

**R8. Degraded authority from witnesses (fixes Codex issue 2).** In degraded mode, p's authority is held from a
majority of witnesses, so it stays bounded (G-33).
- **W1, the grant rule.** A witness `w` grants to `(t, p)` only when all three hold:
  - its highest accepted version of R has term `t`;
  - that version names primary `p`;
  - that version is in degraded mode.

  Once `w` has accepted any version with a higher term, it grants to `t` never again (accepted values only move
  forward in Paxos).
- **Holder rule.** p's degraded authority at p-time `x` is valid only if a majority of witnesses each granted a
  request that p sent at `s` with `x < s + d·(1-ρ)`. R5 applies.
- **Barrier.** This is R3's third bound: the new primary waits W after learning its promotion was chosen.
- **Why this fences any concurrent promotion [INF].** Let promotion `P` (to `t+1`) be chosen at real time `c` by
  accepting majority `Q_a`. Suppose p relies on majority `Q_g` at real time `x ≥ c`. Some witness `w` is in both.
  - If `w` granted after accepting `P`, W1 forbids it.
  - If `w` granted at `g` before accepting `P`, then `g ≤ c`, so that grant supports p only until `g + d ≤ c + d`.
    The new primary serves no earlier than `learned + W ≥ c + d`.
  - So p's authority ends before the new primary's begins. Before `c`, p is the only primary chosen for its term.
- **Interleaving** (degraded, term 5, d = 2, ρ = 0.1):
  1. Witnesses w1 and w2 grant to p at 10.01, so p's authority runs to 11.80.
  2. An operator promotion to term 6 is accepted by w1 and w3 at 10.51, and is chosen then.
  3. **Without the barrier,** the new primary serves from 10.52 while p serves until 11.80.
  4. **With it,** the new primary serves from 12.72.
  5. p's renewal at 11.00 is refused by w1 and w3 (W1). Only w2 still grants, which is not a majority.
- **Witness restart.** The accepted state is durable, so a restarted witness still applies W1. Grants live only in
  the holder's validity window, so the witness needs no grant bookkeeping.
- **Leaving degraded mode.** p keeps the witness lease until the grow version is chosen and p holds grants from every
  member of A.
- **Cost (UNMEASURED).** Consensus renewals happen in every degraded and post-failover window, and the barrier adds W
  to every failover. X-3 measures both.

**R9. Standby read leases.** A standby read lease that p grants never outlasts p's own authority (fixes M2).
[Revised 2026-11-17 after Codex's review of PRs #151 and #152 (`docs/handoff/PR151_152_Codex_Review.md`, R9-A to
R9-E), from the targeted model `spec/tla/StandbyLease.tla` (`spec/tla/RESULTS_R9.md`). **R9-A to R9-E are
CONDITIONALLY FROZEN (owner decision 2026-11-17, `docs/handoff/PR155_Owner_Decision.md`)**, with G-46's clock
assumptions and R9-E's argued formulas accepted. R9-F is not frozen. This is the contract only: not C++ conformance,
not a Rust authorization.]
- **R9-A. Apply before acknowledging.** Every replica (a member of A, or one catching up to join it) reports an
  entry as acknowledged only once it has applied it, whether or not it holds a lease at that moment. p counts no
  other report. An acknowledgement sent before a lease was granted can be counted
  while that lease is valid (RESULTS_R9 M_ackLeased, M_ackDurable).
- **R9-B. Who may hold a lease.** p grants a standby lease to `m` only if `m`:
  - belongs to A;
  - has persisted p's current term (R4);
  - is positioned on p's log (R6.2, R6.3; under R2′ this is rule 2);
  - has no pending truncation intent (R6.3′ item 3);
  - has applied every entry up to `max(inh_p, ack_p)`, judged by the applied position `m` reports (R9-A). `inh_p` is
    p's log end after its quiescence and drain (S2′). The Grow test of R1, applied to the applied state.
  - **Definition of `ack_p`** (added 2026-11-17 for the conditional freeze, `docs/handoff/PR155_Owner_Decision.md`):
    `ack_p` is the highest LSN that `p` has recorded as HA_DURABLE in its current term. That means every member of A
    has reported every entry up to it as applied (R9-A), and `p` held authority when it recorded it. It is 0 when `p`
    starts serving its term. It is never set from `inh_p`, because the inherited log is covered by the `inh_p` term of
    the max. The durability class a client asked for (APPLIED or LOCAL_DURABLE, G-11) does not move it: such a reply
    can be sent before the entry is HA_DURABLE. "Acknowledged" in G-30 means acknowledged at HA_DURABLE.

  Evidence: M_leaseSync, _pack, _inh. The "positioned" part gave no counterexample in RESULTS_R9 (today's R2) and is
  kept. Under R2′ it is needed for the stronger `LeasedPrefix` property (`spec/tla/PromoteRecon.tla`, run MP).
- **R9-C. Removal from A and acknowledgements.** While a lease that p granted to `m` may still be valid, p does not
  remove `m` from A (M_dropWait). Every acknowledgement needs every member of A (M_ackA). These are two separate
  requirements, and each fails alone. "p does not degrade while the lease may be valid" follows from them in the
  A-based protocol, because degrading changes S, not A. It is kept as an explanation, not as a separate mechanism
  (M_shrinkWait gave no counterexample). In today's C++, which has no A, degrading is that removal.
- **R9-D. Term fencing.** R9 relies on R4's holder rule: a standby stops serving lease reads once it persists a
  newer term. The model needs it only with outside-S promotion (M_stopOnLearn_L5, _L6); the in-contract lanes pass
  without it, and `PromoteRecon.tla` run MS did not show it necessary either. It is kept because R4 already states it.
- **R9-E. Timing.** The standby's lease and the cap are anchored at the standby's request-send time, on the
  standby's clock. p's wait is anchored at p's own grant-send time, on p's clock. Never mix the two clocks.
  - **Standby:** it sends the request at `s_m` (its clock). The lease ends at `s_m + min(d·(1-ρ), r·(1-ρ)/(1+ρ))`
    on its own clock, rounded down, where `r` is the cap.
  - **Cap (M2):** `r` is p's remaining authority at grant time, measured on p's clock and sent in the grant as a
    duration. The factor `(1-ρ)/(1+ρ)` converts it safely to the standby's clock, whatever the delay in flight,
    because the standby counts from its send time, which precedes the grant [INF, derived after review; not
    checked].
  - **p's side:** "may still be valid" lasts until `W = d·(1+ρ)` has passed on p's clock since p last sent `m` a
    grant (section 0, waiter side). The model shows that a shorter wait breaks G-30 (M_rho). This bound survives a restart
    of p: a restarted p is no longer primary until a new promotion (R6.0), and the new primary waits out every
    earlier lease through the R3 bound (M_restartStop shows the failure without R6.0).
  - The drift assumption is G-46's: each clock's rate is within `[1-ρ, 1+ρ]`.
  - A standby's lease does not survive the standby's own restart (the suspend-aware clock resets at reboot).
- **R9-F. Reads expose only acknowledged entries.** [Added 2026-11-17 after Codex's review of #153
  (`docs/handoff/PR153_Codex_Review.md`), revised after this step's review (`docs/AFR_001_RECON_BLOCKERS.md`
  section 4). PROPOSED; **text only, not model-checked**. Not part of the R9-A to R9-E set that may be reviewed for
  freeze. It needs its own model with separate applied, acknowledged and `c_m` positions before any freeze.]
  - **`ack_p`** is as defined in R9-B. The inherited log becomes covered once every member of A has applied through
    `inh_p`, as a new Raft leader commits its no-op entry [EXT].
  - `p` sends `ack_p` with every frame, and sends a frame within a stated bound after `ack_p` moves, also when idle.
    `m` keeps `c_m`, the highest value it has received from `p` in `p`'s term while positioned. `c_m` is volatile.
    It resets to 0 when `m` persists a newer term (R4), restarts, or is no longer positioned.
  - **The read's target is fixed when it executes.** `m` keeps, for every key it serves, the LSN of the last applied
    change, deletions included, as tombstones that stay until `c_m` passes them. The same holds for each index entry
    and each range a read depends on. A read captures its answer and the largest such LSN it depends on. It then waits
    until `c_m` reaches that LSN. Then it checks that the lease is still valid and that `m` has not persisted a newer
    term (R5, R9-D), and only then answers with what it captured. If the lease ends first, the read is refused.
  - So `m` never answers with an unacknowledged change (a value, a delete or an index entry), which may later be
    truncated. It never answers with a version older than an acknowledged one, which would break G-30. Later writes
    to the same key cannot starve a read, because the target was fixed at execution.
  - A multi-object read waits for each object it read. It is not a snapshot at one LSN; G-29 says what it promises.
  - The wait is needed because of R9-A: `m` applies an entry before it reports it, so an entry already acknowledged
    to a client can be applied at `m` before `m` learns that it was acknowledged.
  - Cost: nothing on the acknowledgement path. A read of a just-written object may wait up to the frame bound.
  - Prior art [EXT]: Raft followers learn the commit index from `leaderCommit` in every AppendEntries (Ongaro and
    Ousterhout 2014, Figure 2), and a ReadIndex read waits until the applied index reaches the read index (Ongaro's
    dissertation, section 6.4). Kafka's follower fetching (KIP-392) serves only up to the follower's high watermark
    and accepts that it lags; R9-F waits instead, because G-30 promises freshness.
  - R9-F does not replace R9-B's "positioned" clause. `LeasedPrefix` is kept as a model property.
  - **Today's C++ does not do this (ABSENT).** A standby installs records on receipt, before they are acknowledged
    (`shard.cpp:2189-2195`). Installed objects carry `last_lsn = 0` (`:1812`), so a standby read never waits
    (`:626-627`) and checks only the lease (`:555`). A replicated delete erases the object at once (`:1785-1788`).
    The records frame has no acknowledged-LSN field (`replication.h:41-46`), and the primary sends no heartbeat of
    its own. These are D-06 format items: per-key applied LSN, tombstones, a safe LSN for objects loaded from a
    standby's checkpoint (which may hold unacknowledged versions), an index that serves only covered entries, and the
    `ack_p` field with its frame bound.
- Today's C++ blocks a degrade until the lease it recorded expires (`shard.cpp:2254-2258`), but it has no A, its lease
  frame is forgeable, and its bound lacks the `(1+ρ)` factor and is lost on restart (G-30 evidence).
- It is fenced against a new primary by the R3 bound.

## 1. Claim L: at most one node holds valid authority (Codex point 1)

**Drift condition.** Safety requires `r_j / r_p ≤ (1+ρ)/(1-ρ)`, where `r_j` and `r_p` are the two clocks' real
rates [INF].
- This holds whenever both rates are inside `[1-ρ, 1+ρ]`.
- Beyond the condition the two leases overlap, and nothing detects it. For example, with `r_p = 0.85` and
  `r_j = 1.15` there are about 0.2 s with two holders.
- This is an assumption, not a guarantee (G-46). [EXT: Ongaro thesis 6.4.1; etcd raft; CockroachDB self-fences on
  observed offset.]

| # | Case | Verdict | The step that prevents it |
|---|---|---|---|
| 1 | Old primary paused past expiry, resumed with requests already admitted | PREVENTED | R5 alone: the lease is checked after the read state is captured. In normal mode, a write also needs the new primary's copy, which it refuses (R4). In degraded mode (A = {p}) only R5 applies |
| 2a | Grantor restart, then a grant during the boot wait, then promotion (Codex issue 1) | PREVENTED by the fix | R3: `learned + W`, which every earlier grant precedes (interleaving in R3) |
| 2b | Holder restart | PREVENTED | R4 persisted term; R6.0 |
| 2c | Promoted member restarts before it serves | PREVENTED | R6.0: it does not serve that term at all |
| 3a | Set change racing a promotion, both from one version | PREVENTED | Compare-and-set exclusion (today: `snc_witness.cpp:423-513`, `:852-872`, TESTED `test_witness.py:240-256`, [FACT-SNC]) |
| 3b | Grow chosen, reply lost, then promotion (B1) | PREVENTED by the fix | R1.2 |
| 3c | Fast double promotion (B2) | PREVENTED by the fix | R2 READY rule plus R3 |
| 4 | Drift at ρ / beyond the condition | SAFE / out of contract | The drift condition above |
| 5a | Operator promotion inside S during a valid lease | PREVENTED | R3: the `learned + W` barrier |
| 5b | Operator promotion outside S, degraded (Codex issue 2) | PREVENTED by the fix | R8 (W1 plus the barrier) fences the degraded holder. R7's READY condition fences older holders, which needs a STONITH assertion otherwise |
| 5c | Operator promotion outside S while `S = {p, j}` and `j` is partitioned from the operator but still granting to p | **FORBIDDEN** (closure phase) | R7 admits it only once every member of S is confirmed fenced; otherwise it is an unsafe override outside the contract. TLC: `A_mut_5c` reproduces the failure when admitted, `A_fence_op` checks the gate |
| 6 | A standby still serves reads under a lease from the old primary (M2) | PREVENTED by the fix | R9 |

**Option for 5c (not adopted).** Members could grant only while they have confirmed the current term from a witness
majority within `d`. That closes 5c, at the cost of witness traffic in normal mode, and it is unanalysed [HYP].

**Residual assumptions:**
- the drift bound;
- a clock that counts through suspend (today's CLOCK_MONOTONIC does not; `runtime.cpp:38-41`, [FACT-SNC]);
- a fencing action that reports success only when the member has really stopped, and the boot read (R7).

## 2. Claim D: log prefix and acknowledged-write preservation (Codex points 2 and 4)

**These are inferences, not proofs.** Section 6 checks them with TLC within small bounds, and some runs did not complete.

### Definitions

- **Caught up.** `c` is caught up when two conditions hold:
  - its log is a prefix of p's log, as established by R6.2;
  - its durable end is at least **max(p's log end when p learned its promotion, p's highest acknowledged LSN in
    its own term)**, at the moment of the compare-and-set that adds it to S.

  `c` stays in A from then on.

  Revision 2's draft used "p's acknowledged LSN" alone, which is wrong. A new primary has acknowledged nothing in
  its own term, so an empty member could join S, and a later promotion of that member loses every write
  acknowledged in the earlier term. The independent QA found this and TLC confirmed it.

  Both values are local to p. Every acknowledged entry of an earlier term is in p's inherited log (step 2 below).
- **`end_X(t)`.** The last LSN in log `X` whose term is ≤ `t`.

### Invariants

- **I1. Entry uniqueness.** Two entries with the same `(term, lsn)` are identical. This follows from R6.0, since
  each term has one creator that never restarts within the term.
- **I2. Log matching.** If two members that are not incomplete hold the same `(term, lsn)`, their logs agree on
  every LSN up to it.
  - By I1 and induction, a member's entries of term `s` are a contiguous run of the term-`s` primary's log,
    starting where that primary started term `s`.
  - The run starts there because a member accepts entries from a primary only after R6.2 has positioned it on that
    primary's log.
- **I3. Truncation never removes a common entry.** Let `(s, x)` be any entry common to `j` and `k`.
  - `s ≤ t_k` and `s` is in `H_j`, so `s ≤ t*`. Therefore `x ≤ end_j(t*)` and `x ≤ end_k(t*)`.
  - Each truncation point of R6.2 is therefore at or above the longest common prefix.
  - When R6.2 stops, `k`'s last entry `(s, x)` has `s` in `H_j` and `x ≤ end_j(s)`. By I2's contiguity, `j` holds
    `(s, x)`, so `k` is a prefix of `j`.
  - Each round either shortens `k` or stops, so the iteration terminates.
- **I4. Acknowledged-write preservation.** If `e` was acknowledged in term `t`, then:
  - every primary of every later chosen term holds `e`; and
  - every member of S in any chosen version after `e`'s acknowledgement holds `e`.

  **Exception:** R7 outside S.
- **I5. Crash atomicity of truncation and reseed.** After any crash, recovery leaves `k` in one of three states:
  - its pre-truncation log;
  - its truncated log;
  - incomplete.
  It is never left with a truncation half applied, or with new entries above a pending intent.

### Argument for I4 by induction over terms [INF]

1. **Within term t.** When `e` is acknowledged, every member of A has `e`.
   - Every member of S at a later version in term `t` was either in A at the acknowledgement (A ⊇ S, R1), or joined
     later after catching up past the acknowledged LSN.
   - So every member of S holds `e`.
2. **Promotion to t+1.** `j` is in S (R2), so `j` holds `e`.
   - `j` never truncates its own log as primary.
   - R6.1 orders `j`'s new entries after `e`.
3. **Inside term t+1.** A member joins S only after catching up to `j` by R6.2.
   - By I3, its log is then a prefix of `j`'s log.
   - Its end is at least `j`'s inherited log end, and `e` lies within that inherited log (step 2).

   So it holds `e`.
4. **Repeated promotions,** including a `t+2` promotion before `j` writes anything: R2 needs the new primary to be in
   S at `t+1`, which needs step 3. The induction carries on.
5. **Interrupted truncation.**
   - A crash before the intent changes nothing.
   - A crash after the intent re-applies it, idempotently.
   - A crash after the intent is cleared has nothing pending.
   - No entry beyond `L` is accepted before the clear, so I5 holds.
   - Truncation removes only entries above the common prefix (I3), and `e` is common to `k` and `j` whenever `k`
     holds it. So truncation never removes `e`.
6. **Interrupted reseed.**
   - The flag is durable before anything is discarded, so recovery never mixes old state with new.
   - An incomplete member is outside A and S and cannot be promoted, so it is never the only holder of `e` that
     counts.

### Where I4 does not hold

- every copy of `e` is damaged at once;
- single-copy windows (section 3);
- R7 outside S;
- a member whose `H` is lost and is not reseeded (R6.4 forbids this; Kafka's history-loss caveat in KIP-279 [EXT]);
- an implementation that breaks any rule above.

### Unacknowledged suffixes

- Entries the new primary holds become committed once the new set acknowledges them [EXT: PacificA section 2.4].
- Entries it lacks are discarded by I3.
- No client received an OK for these entries, so they are in-doubt (G-58).

## 3. Single-copy windows (Codex point 3)

**Two questions, kept separate:**
- **Admission:** will SNC acknowledge a new write?
- **Preservation:** how many copies does an already-acknowledged write have?

`m` controls only admission.

Revision 1 said `m = 2` "keeps every guarantee". That was wrong. After a promotion, S = {j} (R2), and the old primary
may be gone. Every HA_DURABLE write acknowledged before the failure then survives on `j` alone until a new member has
caught up. `m = 2` refuses new writes in that window. It cannot restore redundancy for old ones.

| Situation | New writes (admission) | Writes acknowledged earlier (preservation) |
|---|---|---|
| Normal, `\|S\| ≥ 2` | HA_DURABLE | Every member of A has them |
| Window with `m = 1` | Acknowledged as LOCAL_DURABLE and reported (G-12). G-11 must say HA_DURABLE requires `\|S\| ≥ 2` | One copy. Loss of that host or disk loses them |
| Window with `m = 2` | Refused | **Also one copy.** Refusal does not help them |
| After R2 discards `k` from S | Same as the rows above | `k` may still hold an intact copy, but it is not promotable until it catches up again. Keeping members whose log is a prefix of `j`'s would shorten this window [HYP, unanalysed] |
| G-14(a) during a window | — | Holds only after AFR-F-01 is fixed. VIOLATED today |
| G-14(b) during a window | — | **No.** A silently lost newest flush on the only copy is not detected |

**Exposure.** The window lasts from the failure until a new member is caught up and the grow version is chosen (R1).
Its length is bounded by catch-up time, which nobody has measured. X-1 adds it.

**Status reporting.** Status should report "redundancy degraded" for existing data during every window, separately
from the class of new writes.

[EXT: Kafka's `min.insync.replicas` limits admission only; KIP-101 Appendix (a) documents that divergence remains
possible with min.isr = 1 and unclean election.]

**The owner decides:**
- the default for `m`;
- whether a window is reported per answer (that waits for AFR-D-14) or only on the status surface.

## 4. Experiments X-1 to X-3

**No latency target exists in any SNC document.** cody sets every threshold below before any run, per AGENTS.md
preregistration. Runs use the R640 with the frozen manifest.

| Experiment | Depends on | Runnable before implementation? | Proposed acceptance |
|---|---|---|---|
| **X-1** slowest-member cost and catch-up time | Today's pairs, run in netns/veth with `tc netem` on the standby link | Yes for N = 2 | Healthy-pair p99 write latency meets cody's target. With the standby slowed by δ, report extra p99 latency at queue depths 1, 16 and 128, the time to remove the slow member, and the **catch-up time** that bounds the section 3 window |
| **X-2** watermark and nonce cost | A micro-benchmark of dsync writes on the R640 data disk | Yes | A per-flush watermark is the single-node default only if its cost stays within cody's bound (proposed: 10%) |
| **X-3** authority overhead and failover delay | A prototype of R1-R9, which needs an implementation go | No | Renewal overhead within cody's bound (proposed: 1%), with R8 witness renewals counted separately. Compare failover against today's partition case (7.7-8.5 s, `REAL_MACHINES.md:239`) and kill case (1.00 s, `:204`). The R3 barrier adds W after the promotion is chosen, and the READY wait adds W to each window. Measure both |

## 5. What this establishes

**Established [INF]:** with R1 to R9 and the residual assumptions, the hand argument finds no interleaving that
breaks Claim L or I1-I5, except the stated out-of-contract cases. Case 5c is now forbidden by R7 (closure phase).

**Not established:**
- that no interleaving was missed (section 6);
- that the C++ implements any of this (it has no primary lease, no ack-set rule for N ≥ 3, no term stamps and no
  term-history reconciliation);
- the costs.

## 6. Validation: TLA+ model

The model is in `spec/tla/` (see `spec/tla/README.md`). It is a verification artifact, not product code.
- **Switches.** Each earlier failure has a switch that removes its fix, so the checker must reproduce the
  counterexample: B1, B2, M1, M2, Codex issues 1 and 2, interrupted truncation, and reseed.
- **Configuration.** The fixed configuration must pass within the stated bounds.
- **Results.** The checker's output is recorded in `spec/tla/RESULTS.md`, with every counterexample reported as found.
  It includes the modelling limits, which mean a passing run proves less than the full protocol.
- **Summary of the results** (bounded, see `RESULTS.md` for bounds and traces):
  - every fix switched off reproduces its failure, except M1. Codex issues 1 and 2, B1, B2, M2, T1, T2,
    interrupted truncation, reseed, the incomplete flag, R6.0, R7 and case 5c all give counterexamples;
  - **M1 is not reproduced.** Without the KIP-279 iteration the reachable state space is unchanged, so R6.2 is not
    validated by the model and rests on the argument alone;
  - the fixed Log model passes exhaustively (MaxTerm 3, three entry ids, one crash);
  - the fixed Authority model passes exhaustively one scenario at a time (restart, standby leases, operator
    promotion outside S) at MaxTime 4 and MaxTerm 2. **Degraded mode and the all-scenario configuration did not
    complete** within their time limits, with no violation in what was explored.
- **Deterministic tests** (once implementation is authorized): a kill at each step of R1, R3 and R6, and the
  scenarios in CORE_DECISIONS 5.2.
- **The history checker** from SNC_GUARANTEES section 15.

## 7. Revision 2 changes (Codex review of PR #142)

1. **R3 grantor restart.** Eligibility is now `learned + W`. Every grant to a lower term, including grants after a
   restart, precedes `learned`. There is a concrete interleaving in R3. The value of W was also corrected
   (section 0).
2. **R8 witness lease.** W1, the holder rule, the linearization point (chosen) and the barrier (learned + W) are
   specified, with an argument and an interleaving. This also resolves 5b. 5c stays UNRESOLVED.
3. **Single-copy windows.** Admission is separated from preservation. `m = 2` does not protect earlier writes.
4. **R6.** KIP-279 was verified against the original page (Accepted, KAFKA-6361, page version 13):
   - **Leader's answer:** "the largest epoch less than or equal to the requested epoch (LeaderEpoch) and the end
     offset of this epoch".
   - **Follower's iteration:** it repeats "until the follower receives the epoch it knows about".
   - **What neither page states:** KIP-279 and KIP-101 give no prefix or commit-preservation invariant, and do not
     cover interrupted truncation or reseeding. Section 2 argues those for SNC.
   - **New rules:** R6.0 (term ownership, matching KIP-101's new-epoch-on-restart rule), R6.1, R6.3-R6.6, and
     invariants I1-I5.

**Found while making Revision 2.**
- **By TLC:**
  - T1: no checkpoint while an intent is pending (R6.3);
  - T2: the R1.2 read must still be at p's own term.
- **By the independent QA:**
  - the "caught up" definition (blocker, section 2);
  - R7 needs READY for an outside-S promotion from degraded mode;
  - R3's first two bounds are redundant;
  - case 1's rationale in degraded mode;
  - R9 has no clock rule;
  - model-fidelity limits, now listed in `spec/tla/RESULTS.md`.

## Sources

**Opened by the reviewer:**
- PacificA, sections 2.2 to 2.4: https://www.microsoft.com/en-us/research/wp-content/uploads/2008/02/tr-2008-25.pdf
- Ongaro's thesis, chapter 4, section 6.4.1 and Appendix B: https://raw.githubusercontent.com/ongardie/dissertation/master/online.pdf

**Opened for Revision 2:**
- KIP-279: https://cwiki.apache.org/confluence/spaces/KAFKA/pages/75978352/KIP-279+Fix+log+divergence+between+leader+and+follower+after+fast+leader+fail+over
- KIP-101: https://cwiki.apache.org/confluence/display/KAFKA/KIP-101+-+Alter+Replication+Protocol+to+use+Leader+Epoch+rather+than+High+Watermark+for+Truncation

**Opened in the PR #140 survey** (`AFR_001_CORE_DECISIONS.md` section 7):
- Aurora: https://pdos.csail.mit.edu/6.824/papers/aurora.pdf
- TiKV lease read: https://tikv.org/blog/lease-read/
- PostgreSQL failover: https://www.postgresql.org/docs/current/warm-standby-failover.html
- Vertical Paxos: https://microsoft.com/en-us/research/uploads/prod/2009/08/Vertical-Paxos-and-Primary-Backup-Replication-.pdf
- etcd raft: https://pkg.go.dev/go.etcd.io/raft/v3
- CockroachDB replication layer: https://docs.cockroachlabs.com/docs/stable/architecture/replication-layer
