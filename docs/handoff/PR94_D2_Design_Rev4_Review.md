SNC Server OS — PR #94 Review / D.2 Implementation Authorization

(Filed verbatim from cody's message in the project thread, 2026-11-10 07:52 UTC. HTML entities decoded.)

Decision

PR #94 — D.2 design rev4: APPROVED.

D.2 IMPLEMENTATION: AUTHORIZED.

PR #94 closes the two remaining correctness blockers from the PR #93 review without reopening the already-approved D.2 architecture.

The D.2 design is now sufficiently specified to implement.

Start from current "main" after PR #94:

"33f7c6df4760965636c95131a64170814f8eb8b9"

Do not reopen the rev1–rev3 architectural decisions unless implementation uncovers a concrete contradiction. If a confirmed defect is found while implementing this authorized slice, fix it on site and document it.

---

PR #94 review

1. SYNC accounting barrier — APPROVED

Rev4 correctly fixes the remaining stream-ordering defect.

The required invariant is now explicit:

«A control snapshot may not overtake data whose accounting effect it already reflects.»

Frames already encoded into "NodeLinks::Out::buf" have already performed their accounting transition. Therefore:

- kernel-buffered bytes remain ahead of SYNC;
- bytes already in "Out::buf" remain ahead of SYNC;
- SYNC is appended FIFO behind them;
- SYNC may preempt only higher-level work that has not yet been pushed/accounted;
- "k", "rx", and applicable END state are sampled at that append barrier.

This is compatible with the existing "NodeLinks::push()" / "flush()" structure and closes the double-decrement failure identified in the PR #93 review.

Keep X28 and ND36 exactly for this invariant.

2. Persistent incarnation generation — APPROVED

Replacing "CLOCK_REALTIME + random" with a durable monotonically increasing generation is the correct design.

Required properties from rev4 are approved:

- u64 generation;
- durable allocation before listener or Out connections become visible;
- checked increment;
- temporary file → file "fsync" → atomic rename → directory "fsync";
- allocation serialized under the engine/data-directory locking discipline;
- corrupt/missing generation state in a non-empty data directory fails closed;
- no wall-clock dependency;
- in-memory tests use a monotonic test allocator;
- peers accept only newer incarnations.

The crash-order argument is sound because an incarnation is not exposed on the wire until the durable update is complete.

Keep X29 and ND37.

3. Restored-data-directory recovery — APPROVED WITH FAIL-CLOSED SEMANTICS

Approve "--links-generation-min G" for v0.1.

It is an explicit operator recovery mechanism, not automatic healing.

Requirements:

- never automatically accept a lower incarnation;
- never silently reset the generation;
- apply the requested floor durably before allocating the first new incarnation;
- the resulting wire generation must remain greater than the durable floor;
- retain/report the peers' highest-seen incarnation so the operator can establish an appropriate floor;
- log the recovery action clearly;
- if a safe floor cannot be established, remain unavailable rather than guess.

Safety takes precedence over availability here.

4. Minor documentation correction

Rev4 changed incarnation creation from drawing a clock/random value to allocating a durable generation. Any leftover wording such as section 3.5 saying a restarted instance "draws a new incarnation" should be corrected during the implementation PR to "allocates/persists a new incarnation generation."

This is editorial, not an implementation blocker.

---

Authorized next task: implement D.2

Implement only D.2 cross-node exchange accounting from "docs/CROSS_NODE_EXCHANGE_DESIGN.md" rev4.

Use the implementation order already specified in section 9. Keep it as one reviewable PR, with the implementation commits in this order and each intermediate commit green:

1. Wire v2:

   - 48-byte Hello;
   - flags;
   - connection nonce / sequence;
   - HelloAck;
   - DATA / END / SYNC / HEARTBEAT kinds;
   - version/accounting mismatch refusal;
   - behavior unchanged until later commits enable accounting semantics.

2. Invariant M:

   - listener before Out links;
   - data-directory lock;
   - persistent incarnation generation;
   - peer generation enforcement;
   - In replacement per member;
   - fencing rules.

3. Heartbeats:

   - pause state;
   - blocked bit;
   - silence detection;
   - member steering.

4. Accounting entries and binding:

   - per-instance entries;
   - UNBOUND / IDENTIFIED / SYNCED / BOUND;
   - same-socket HelloAck identity proof;
   - unbound-as-down behavior;
   - exact current-connection SYNC;
   - FIFO SYNC accounting barrier;
   - "resume_all" only at successful binding;
   - no counted DATA before BOUND.

5. END:

   - coalesced per-entry END debt;
   - dispositions;
   - retirement;
   - stale-target handling;
   - underflow detection.

6. Quarantine:

   - In-down trigger;
   - blocked trigger;
   - not-bound trigger;
   - Out reset before debt stops gating;
   - quarantined debt is non-increasing.

7. Drop-to-resume:

   - no adoption;
   - correctly END/drop work whose counting instance cannot follow it;
   - recreate through existing durable state/resume machinery.

8. Blocked primary:

   - leave node links at "T_block";
   - preserve "blocked_since_";
   - rejoin as a new links instance/incarnation.

9. Gauges and counters exactly sufficient to prove the D.2 invariants and diagnose failures.

10. Positive and negative tests.

11. Evidence and documentation.

Do not collapse the implementation into a single opaque commit. The commit sequence is part of the review evidence.

---

Required validation

The implementation is not complete because the happy-path tests pass.

Run the complete D.2 test program, including the adversarial cases from the design:

- X1 through X29, including X19b;
- ND1 through ND37;
- every existing D.1 test unchanged;
- F8 cross-node sender-gate regression;
- full Release serial suite;
- full ASan + UBSan serial suite;
- full TSan serial suite;
- P2 on the development VM as a regression/overhead measurement only, not as hardware tuning evidence.

In particular, do not weaken or simulate away the important failure tests:

- X3/X4 exact accounting evidence at the SYNC barrier and apply point;
- X14 repeated one-way quarantine cycles;
- X17 repeated churn while checking the structural bound at every sample;
- X19/X19b blocked-primary timing;
- X24 repeated forward flaps before reverse reconciliation;
- X25 delayed frames from the retired incarnation;
- X27 SYNC arriving before HelloAck;
- X28 already-accounted frames queued ahead of SYNC;
- X29 restart generation under same-time, backward-clock, rapid-restart, crash-at-update-stage, and corrupt-generation cases.

ND36 must prove that putting SYNC ahead of already-accounted "Out::buf" data deterministically breaks X28.

ND37 must prove that the old "CLOCK_REALTIME + random" incarnation scheme fails X29.

Retain all test attempts in chronological order. Do not discard an initial failure and report only the clean rerun. A failure gets explained, fixed on site if it is within D.2, and rerun with the original evidence preserved.

---

Evidence required in the PR

The PR description/handoff must make it possible to verify the protocol, not merely state that tests passed.

For binding/reconciliation evidence, capture enough state to demonstrate:

"count = k + pushed - R"

for applicable current-connection reconciliation, and at initial binding with no counted DATA sent:

"pushed = 0"
and therefore
"count = k".

Record the relevant origin and peer values at the SYNC accounting barrier and when the SYNC is applied.

Also show:

- zero "xnode_end_underflow";
- stale SYNCs rejected rather than applied;
- no counted DATA before BOUND;
- debt cannot increase while quarantined;
- entry count remains structurally bounded by member count;
- repeated failure/churn does not grow hidden state;
- delayed old-incarnation frames cannot mutate the new entry;
- every legitimate new incarnation is greater than its predecessor independent of wall clock;
- eventual quiescence returns applicable counts/debt/held state to zero.

Run the structural B2 bound assertions during the churn tests, not only at final quiescence.

---

Scope boundary

Do not start any of the following in this PR:

- S5;
- S6;
- R640 campaign work;
- CP-1A rerun;
- comparators;
- B2 begin-snapshot work;
- Agent-SNC;
- Rust rewrite;
- DPDK/VPP;
- any change to the reply-only follow-up contract;
- any change to X admission semantics.

Do not opportunistically refactor unrelated networking or HA code while touching "node_link.*", "shard.*", or "engine.cpp".

KEEP the approved architecture. REFINE only where implementation requires the rev4 protocol. RECONSIDER something only if there is concrete contradictory evidence.

---

Records

Before or as part of the implementation work:

1. File this review as the PR #94 D.2 design rev4 review handoff.
2. Update "docs/CROSS_NODE_EXCHANGE_DESIGN.md" from "rev4, for review / no code" to reflect rev4 approved / D.2 implementation authorized.
3. Update "docs/STATUS.md".
4. Add the corresponding concise "AGENTS.md" handoff entry.
5. Keep the detailed implementation evidence in the implementation PR/results rather than bloating "AGENTS.md".

---

Completion gate

When the D.2 implementation PR is ready, stop there and return it for independent review.

Do not call D.2 complete and do not move to S6 simply because the implementation compiles or the initial suite is green.

The review needs to verify:

- implementation against rev4;
- accounting equations;
- failure/reordering behavior;
- structural boundedness under repeated churn;
- negative-test sensitivity;
- sanitizer results;
- evidence provenance.

Standing SNC rules remain:

Always verify, never assume.

Fix confirmed defects on site.

Correctness before performance.

Structural bounds must remain structural under repeated failure and churn.

A control snapshot may not overtake data whose accounting it already reflects.

Distributed generation ordering must not depend on wall-clock monotonicity.
