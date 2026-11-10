# SNC Server OS — PR #91 D.2 Design Review Handoff

**PR:** #91 — `D.2 design: cross-node exchange accounting (for review)`  
**Reviewed head:** `56ba0a0c4efd4b0f5c32dcd036bde96b20ca1e0f`  
**Merged as:** `2367c4dba3d2d08500357327a31647109e3d8ec7`

## Decision

**PR #91: DESIGN DIRECTION APPROVED, IMPLEMENTATION HOLD.**

The overall D.2 direction is sound:

- keep a cross-node exchange counted at its origin until it actually ends;
- use END for remote completion;
- use exact reconciliation rather than the old generation-reset sketch;
- keep the DATA frame at 64 bytes;
- fail closed on the v2 wire;
- preserve D.1 read-pause/backpressure semantics;
- use per-incarnation accounting;
- expose D.2 through GAUGES;
- validate with fault injection, negatives, sanitizers, and a dev-VM regression check.

However, the current design has several distributed-accounting holes that must be corrected before any D.2 code is authorized.

**Do not implement D.2 from the current note. Produce a design rev2 first and return it for review.**

---

## 1. BLOCKER — an Out connection is not safely bound to the peer incarnation

The note says `inc_out_[l]` is the incarnation reached by o's Out connection and that it is learned from the peer's Hello on the reverse In connection.

That is not a safe identity binding.

The existing node links are one-way. The sender's Out connection does not receive a Hello from the process at the far end. In the exact failure case D.2 is trying to solve, an old fenced primary and a promoted primary can both exist and their reverse In connections can replace one another. Therefore:

**the incarnation observed on the reverse In connection is not necessarily the process currently receiving o's Out socket.**

If `remote_[l]` is attributed to the wrong incarnation, END/SYNC/orphan accounting can be applied to the wrong bucket and the no-double-end/no-under-count proof fails.

### Required correction

Bind an Out connection to a remote incarnation only through evidence that proves that incarnation saw that exact Out connection.

Preferred approach using the proposed protocol:

1. o creates Out connection nonce `conn`.
2. Until the peer proves receipt of that nonce, the Out link is **UNBOUND** for counted DATA.
3. p receives o's Hello containing `conn`.
4. p sends a SYNC/identity control frame back on its reverse Out connection.
5. o accepts the binding only when:
   - the control frame arrived on an In connection whose Hello identifies incarnation `p_inc`; and
   - the SYNC names o's current `conn`; and
   - the SYNC names o's current incarnation.
6. Only then may o set `inc_out_[l] = p_inc` and send counted DATA on that Out connection.

An explicit Hello-ACK on the same transport would also work, but if the existing one-way link design is retained, the exact-SYNC proof is the cleaner route.

**Do not infer `inc_out_` from an arbitrary reverse Hello.**

Add a negative test that deliberately makes old and new peer incarnations' reverse connections alternate while o's Out connection remains attached to only one of them. The wrong-incarnation implementation must fail.

---

## 2. BLOCKER — release is unsafe while o -> p can still carry DATA

Section 2.6 releases `remote_[l]` after o's In connection from p has been down for `T_release`, **whatever o's Out link is doing**.

The same section then argues:

> nothing new reaches p; o's queue to a down link is purged.

Those two statements are inconsistent.

The release condition is loss of **p -> o**. The **o -> p** Out connection may still be healthy. In that one-way-failure case o can continue sending counted DATA to p.

If o then:

- sets `remote_[l] = 0`;
- opens X admission again;
- leaves o -> p usable;

then p can continue accumulating old and new work while o no longer counts the released work. Repeating this can defeat the structural bound D.2 exists to establish.

### Required correction

Release must include a **quarantine/drop transition** for the forward Out link.

Before a released count stops gating admission:

1. mark the pair/link QUARANTINED;
2. stop new counted DATA from being pushed on o -> p;
3. drop/close the current Out connection or otherwise prove that it cannot deliver additional DATA;
4. purge/retain local queued work under the ordinary X bound;
5. move the old remote amount into an explicit released/orphan debt term if needed for the proof/GAUGES;
6. do not reopen counted DATA until the peer is identity-bound again and an exact SYNC has re-established state.

This still solves the availability problem that motivated release: the dead peer no longer holds X forever. New local roots can be admitted under X, but their follow-ups remain locally bounded while the peer pair is quarantined.

Add a specific one-way-partition test:

- o -> p continues to pass traffic;
- p -> o is blackholed;
- run longer than multiple `T_release`;
- prove the amount of physical remote work cannot grow by another X on every release.

The current X5 is not strong enough unless it checks this exact accumulation case.

---

## 3. BLOCKER — incarnation transitions need explicit state rules

The design uses a single `held_for_[l]` and a single `end_owed_[l]`, while END carries an origin incarnation.

That is only safe if the transition between origin incarnations is completely specified.

Consider p holding/ending items for old origin incarnation o1 when a Hello from o2 replaces it. If `end_owed_[l]` contains completions for o1 and then completions for o2 are coalesced into the same scalar, one END frame cannot correctly identify both origins.

The design must explicitly define what happens to every per-link field on an origin-incarnation change:

- `held_for`;
- `end_owed`;
- `rx`;
- `in_conn`;
- pending SYNC;
- queued DATA carrying `origin_link`;
- replies already waiting for durability;
- any orphan/released debt.

### Required correction

Either:

**A. make the peer-side accounting explicitly per incarnation**, or

**B. enforce a single-current-incarnation state machine and, on incarnation replacement:**
- adopt all still-held old-incarnation items before switching;
- retire/clear old `end_owed` safely and leave the old origin conservatively over-counted until its own SYNC/release;
- reset current-incarnation counters atomically;
- never combine END debt from two incarnations.

Whichever approach is chosen must be written as an invariant and tested.

---

## 4. BLOCKER — "one orphan, third incarnation releases oldest" is not structurally safe without a stated invariant

Section 2.7 says only one orphan entry is retained per link and a third incarnation releases the oldest.

Arrival of a third incarnation does **not by itself prove** that the oldest incarnation is dead or unreachable.

If SNC has a structural invariant that at most two live incarnations can exist for a `(group, shard)` pair, state and prove that invariant from the HA/member model and test it.

Otherwise, do not release an orphan merely because another incarnation appeared.

A safe design is to retain bounded per-incarnation entries for every incarnation that can still have a live connection, and release an entry only under the same liveness/quarantine rule used elsewhere.

The number of retained entries must itself have a structural bound.

---

## 5. The B2 proof must include release/adoption terms explicitly

Section 5 says:

`Σ items <= Σ_s live_s`

but the note itself acknowledges a period after release in which remote physical items are deliberately no longer represented in `live_s`.

Therefore that inequality is not true during the release transient.

The final proof needs an explicit term, for example conceptually:

`physical_items <= Σ live_s + Σ released_debt_s`

and then prove a fixed structural upper bound for `released_debt`.

Likewise, `adopted_s` cannot simply appear as an unbounded symbolic term. The proof must state a finite structural maximum or show why repeated flap/incarnation/release cycles cannot accumulate adoption without bound.

This is especially important because the whole purpose of D.2 is to turn B2 into a **structural cluster-wide bound**, not merely an observed one.

The revised test plan should include repeated release/reconnect/incarnation churn, not just a single cycle.

---

## 6. Smaller corrections required in rev2

### Hello size

Section 2.2 says:

> **Hello, 32 bytes**

but the listed layout totals 48 bytes, and the next sentence correctly says the Hello is 48 bytes.

Fix the heading/layout statement before implementation so the wire specification is unambiguous.

### Mixed-version client effect

Section 2.8 says:

> Clients are not affected: node links carry only follow-ups.

That is too strong.

A fail-closed mixed cluster can delay cross-node settlement and can eventually affect admission/availability through X/backlog behavior. The correct claim is that client request/response transport does not use node links; application-visible availability may still be affected while cross-node follow-ups cannot settle.

Document that honestly and test the expected behavior.

### Timer constraints

`T_hb = 100 ms`, `T_dead = 1 s`, and `T_release = 3 s` are acceptable as hypotheses, not tuned defaults.

The configuration should enforce sane ordering constraints such as:

`T_hb < T_dead < T_release`

and the correctness proof must not depend on the exact numerical defaults.

---

## 7. Review answers to section 10

### Q1 — Version policy

**APPROVE fail-closed v2.**

Do not dual-stack v1 in D.2. A v1 fallback quietly gives up the structural bound on those links and makes the proof conditional on peer version.

### Q2 — Release

**APPROVE the need for release, but NOT the current mechanism.**

Release is necessary for availability, but it must first quarantine/drop the forward data path so released remote work cannot continue growing invisibly.

### Q3 — Adoption above X

**CONDITIONALLY ACCEPTED, not yet approved.**

Non-refusable internal work may need adoption, but the design must prove a finite structural adoption term under repeated slot moves, incarnation changes, reconnects, and releases.

### Q4 — SYNC vs generations

**APPROVE SYNC reconciliation.**

The generation-reset sketch has flap amplification and restart identity problems. Exact reconciliation is the better direction.

### Q5 — timer defaults

**ACCEPT as hypotheses only.**

Keep them overrideable and visible in GAUGES. Characterize them later on target hardware.

### Q6 — per-incarnation counting

**APPROVE the direction, HOLD the current mechanism.**

Per-incarnation isolation is required for fenced-primary behavior, but Out-to-incarnation binding, peer-side END debt, and orphan lifetime must be corrected as above.

---

## 8. What remains approved from PR #91

Keep these parts:

- D.1 stays unchanged.
- Cross-node exchanges remain live at their origin until remote end/reconciliation.
- END frames are coalesced control state.
- SYNC is the reconciliation mechanism.
- DATA remains 64 bytes if the corrected design still fits.
- Wire v2 is fail-closed.
- Heartbeats must not fight D.1 read pausing.
- No `TCP_USER_TIMEOUT` behavior that converts legitimate backpressure into churn.
- Accounting/control must not introduce a distributed wait cycle.
- GAUGES are mandatory.
- X1-X12/ND tests are the right style, but add the new adversarial cases above.
- Release/ASan+UBSan/TSan and P2 remain required.
- No S6/R640/CP-1A/comparators/B2-snapshot/S5 scope creep.

---

## 9. Required next action

Create **D.2 design rev2 only**.

Rev2 must:

1. bind each Out connection to the actual receiving peer incarnation using exact proof;
2. quarantine/drop the forward Out path before release can remove remote accounting;
3. define every per-incarnation counter transition, especially `end_owed`;
4. remove or prove the one-orphan/third-incarnation assumption;
5. rewrite the B2 proof with explicit bounded release and adoption terms;
6. add adversarial tests for:
   - reverse-Hello incarnation mismatch;
   - one-way partition across repeated releases;
   - repeated incarnation alternation;
   - old/new END debt at an incarnation switch;
   - repeated adoption/release cycles;
7. fix the wire-size and mixed-version wording.

**No D.2 implementation yet.**

Return rev2 for review before code.

---

## Final ruling

**PR #91: DESIGN DIRECTION APPROVED WITH REQUIRED REVISION.**  
**D.2 IMPLEMENTATION: NOT AUTHORIZED YET.**

The core idea is good. The current note is not yet strong enough to support the structural cluster-wide B2 claim under one-way failure and concurrent/fenced peer incarnations.

Standing rules remain:

> Always verify, never assume.  
> Fix confirmed defects on site.  
> Correctness before performance.  
> Structural bounds must remain structural under failure and churn.  
> Do not advance scope until the reviewed slice is actually closed.
