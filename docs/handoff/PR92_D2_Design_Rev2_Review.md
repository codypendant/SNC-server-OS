# SNC Server OS — PR #92 D.2 Design Rev2 Review Handoff

**PR:** #92 — `D.2 design rev2: cross-node exchange accounting (for review)`  
**Reviewed head:** `64c757a184f26da7e7b7584a1692bfd7663fd2b7`  
**Merged as:** `dcedf7cf29b2ebf7e29f0b420a24521f726187ac`

## Decision

**PR #92: DESIGN REV2 SUBSTANTIALLY APPROVED, IMPLEMENTATION HOLD.**

Rev2 fixes the major architectural defects identified in the PR #91 review:

- Out identity is no longer inferred from an unrelated reverse Hello.
- Release is now a quarantine that resets the forward path before debt stops gating X.
- State and END debt are per peer instance.
- The unsafe "third incarnation releases oldest" rule is gone.
- Invariant M gives a structural entry bound.
- Adoption is removed in favor of drop-to-resume.
- The B2 proof now includes explicit debt/unretired-instance terms.
- Mixed-version behavior, timer ordering, and observability are materially better specified.
- The adversarial test plan is substantially stronger.

The direction is now close to implementation-ready.

However, two correctness blockers remain in the connection-reconciliation state machine. One additional availability/timing correction is also required.

**Do not implement D.2 from rev2 yet. Produce a narrow design rev3 and return it for review.**

---

# 1. BLOCKER — FINAL SYNC arithmetic is internally contradictory and currently wrong

Section 2.5 says an exact FINAL SYNC applies:

`count = k + (pushed - R)`

for the closed connection.

But the same exactness argument correctly states:

> For a FINAL SYNC the connection is closed at f, so `pushed - R` were lost.

Those statements cannot both be true.

If a connection pushed 10 counted DATA frames and the receiver consumed 6 before the connection was finally closed:

- `pushed = 10`
- `R = 6`
- `pushed - R = 4`

Those four frames were **not received** and cannot remain physical work at the peer after the connection is closed.

Adding them back into the exact count preserves the lost work instead of removing it.

That contradicts section 3.1, which says:

> The FINAL SYNC removes the lost items exactly.

## Required correction

The rev3 protocol needs one unambiguous connection-reconciliation rule.

I recommend simplifying the state machine instead of accumulating arbitrary old connection records.

### Recommended state sequence

Use two distinct concepts:

1. **IDENTIFIED** — HelloAck proves which peer instance accepted this exact TCP connection.
2. **ACCOUNTING-SYNCED / BOUND** — an exact current SYNC for that same connection reconciles the entry.

Flow:

1. s connects with nonce `c`.
2. HelloAck on that socket proves peer instance `e`.
3. The link remains **UNBOUND for counted DATA**.
4. e's reverse Out path sends a current SYNC naming `c`.
5. Because no counted DATA has been sent on `c` yet, `pushed_current = 0`.
6. That exact current SYNC reconciles the entire entry:
   - items still physically held by e are in `k`;
   - frames lost on any prior closed connection disappear;
   - prior received items still held are already in `k`.
7. Only after this exact current SYNC does the link become BOUND and run `resume_all`.

This has several advantages:

- no counted DATA is sent before accounting is reconciled;
- no arbitrary history of old connection generations is required;
- rapid forward-link flapping cannot create an unbounded list of unreconciled connection records;
- HelloAck still provides exact peer identity;
- the reverse path still proves accounting state before work resumes;
- if the reverse path is dead, the link remains down-for-queueing and quarantine eventually provides availability.

### FINAL SYNC

FINAL can remain as an optimization/early correction, but its semantics must be explicit.

At minimum:

- a FINAL for a closed connection must **not add `pushed - R` as physical work**;
- a stale FINAL must never overwrite or raise state after a newer exact current SYNC;
- either scope every connection by an ordered per-incarnation connection sequence, or make current exact SYNC supersede all prior FINAL state and ignore older FINALs thereafter.

Do not rely only on random nonces to determine temporal ordering. A nonce establishes identity, not "older/newer".

## Related contradiction that must be removed

Section 2.3 says `out_conn_`, `pushed_`, and `out_closed_` are "zeroed together", while section 2.5 requires the last closed connection's nonce and pushed count to remain available for a later FINAL.

Rev3 must define exactly when those fields are retained, superseded, and cleared.

## Required test additions

Add a test beyond current X3/X4:

### X24 — repeated forward flaps before reverse reconciliation

- keep the reverse control path delayed;
- flap the forward Out connection several times;
- allow multiple old connections to close before any reconciliation reaches the origin;
- then restore the reverse path.

Pass criteria:

- no lost connection generation can resurrect `count`;
- no stale FINAL can increase state after a newer exact SYNC;
- the final exact count equals the physical held/in-flight work;
- `xnode_end_underflow == 0`;
- quiescence reaches zero.

Negative mutations should include:

- FINAL incorrectly adds `pushed - R`;
- stale FINAL applied after newer exact SYNC;
- DATA allowed after HelloAck but before exact current SYNC.

---

# 2. BLOCKER — retirement after "silence" must actively fence the old socket before zeroing the entry

Section 2.12 C says a newer incarnation may be accepted once the older incarnation's In connection:

- has closed; **or**
- has been silent for `T_dead`.

Section 2.7 then retires the old entry and zeros/purges its accounting.

Silence alone is not enough to guarantee that the old TCP connection can never deliver another frame.

Even if Invariant M says the old process should no longer exist when the same member starts a new incarnation, there may still be delayed/kernel-buffered bytes on the old connection. More importantly, the peer-side fallback for a violated M explicitly uses silence as a discriminator.

Once the old entry has been zeroed, a late DATA/END/SYNC from that old socket must not be able to mutate the new entry or create an unaccounted item.

## Required correction

When the `T_dead` silent path is used to admit a new incarnation of the same member:

1. actively close/reset the old per-member In socket;
2. reset any Out connection bound to that old incarnation;
3. remove the old sockets from polling;
4. only after the old transport is fenced may the entry be RETIRED and zeroed;
5. only then accept/activate the new incarnation.

A retired entry's connection must be incapable of producing another accepted frame.

This should become part of Invariant M/M1, not merely an implementation detail.

## Required test

### X25 — delayed old frames after silent replacement

- hold an old incarnation's connection open but silent past `T_dead`;
- start/offer the new incarnation;
- queue delayed old DATA/END/SYNC so that it would arrive if the old socket were left alive;
- admit the new incarnation.

Pass criteria:

- old socket is reset/removed before retirement;
- no delayed old frame reaches the new entry;
- no underflow;
- no uncounted physical item;
- old entry is purged exactly once;
- new entry starts clean.

Negative mutation:

- accept the new incarnation after silence without closing the old socket.

---

# 3. REQUIRED CORRECTION — blocked-primary recovery timing is inconsistent with the written timers

Rev2 says:

- a blocked primary leaves links after `T_block`;
- peers quarantine a blocked entry after `T_release`;
- default `T_block = 2 s`;
- default `T_release = 3 s`.

Section 2.12 then says the promoted member's count may stall admission for at most:

`T_block + T_dead`

and X19 expects recovery within that window.

That does not follow from the written quarantine trigger.

If the old primary leaves its links at `T_block`, but its old count remains gating until the In-down quarantine timer reaches `T_release`, admission can remain affected for approximately:

`T_block + T_release`

unless the blocked timer continues across socket destruction and quarantine is measured from the original blocked timestamp.

## Required correction

Pick one rule and state it precisely.

Preferred:

- maintain `blocked_since` for the entry;
- destroying the old links does not reset that timer;
- quarantine occurs when either:
  - blocked duration reaches `T_release`, or
  - In-down duration reaches `T_release`;
- member steering may move transport earlier, but old accounting remains gating only until the defined quarantine deadline.

Then make X19's deadline match the actual state machine.

This is primarily an availability/liveness correction, not a safety blocker, but the test and design must agree before implementation.

---

# 4. PR #91 blockers — status after rev2

## Out connection identity

**Resolved in direction.**

HelloAck on the same accepted TCP connection is a sound way to prove which instance accepted that specific Out connection.

Keep it.

The remaining correction is that HelloAck should establish identity, while an exact current SYNC should establish accounting readiness before counted DATA starts.

## Release while forward DATA continues

**Resolved.**

Quarantine now resets the forward Out connection before the count stops gating X, and re-entry requires both directions.

This closes the repeated-X-per-release hole identified in PR #91.

Keep X14/ND16/ND25.

## Per-incarnation END/held state

**Resolved.**

Per-entry state and per-entry `end_owed_` remove the cross-incarnation debt mixing problem.

The retirement table is the right design style.

Keep X16/ND17/ND18.

## One-orphan / third-incarnation rule

**Resolved in direction.**

The old arbitrary eviction rule is removed.

Invariant M plus per-member entries gives a structural entry count, assuming the deployment model.

The peer-side conflict defense is also appropriate.

## B2 release/adoption terms

**Resolved in direction.**

Removing adoption is a major improvement.

The explicit terms:

- live;
- D (quarantined debt);
- U (dead but not-yet-retired peer-held work)

make the proof materially stronger.

The loose bound is acceptable for v0.1 if the corrected connection reconciliation preserves it under churn.

---

# 5. Answers to PR #92 section 10 questions

## Q1 — Drop-to-resume instead of adoption

**APPROVE.**

This is cleaner than adoption because it preserves the original X admission invariant.

The cost is latency, which is preferable to an unbounded accounting term.

## Q2 — Invariant M

**APPROVE WITH THE transport-fencing correction above.**

Listener-before-links, data-dir lock, member identity, and peer refusal form a reasonable v0.1 basis.

When silence rather than an observed close is used, the old socket must be actively fenced before retirement.

## Q3 — B2 constant

**APPROVE the loose constant.**

A structural loose bound is preferable to a tight empirical one.

Do not optimize this constant until target-hardware evidence shows a reason.

## Q4 — Quarantine

**APPROVE.**

This is the correct replacement for rev1 release.

The forward path must remain incapable of counted DATA until identity and accounting are both re-established.

## Q5 — Timer constraints

**APPROVE as hypotheses.**

The ordering constraints are appropriate.

Correct the blocked-primary timing statement/test.

## Q6 — In replacement per member

**APPROVE.**

The ordering argument is adequate: ordering is per sender instance, and the current per-shard replacement already does not create a meaningful total order across two different members.

## Q7 — HelloAck

**APPROVE.**

Writing one fixed-size identity acknowledgement on the accepted socket is justified.

Treat it as identity proof, not full accounting readiness.

## Q8 — blocked primary leaves links

**APPROVE in D.2 scope.**

This is directly required to bound the fenced-alive behavior that D.2 must reason about. It is not opportunistic HA work.

Keep it narrowly scoped to node-link withdrawal/recreation; do not expand into a broader replication redesign.

---

# 6. Test plan assessment

The rev2 test program is strong.

Keep:

- X1-X23;
- ND1-ND26;
- full Release;
- ASan+UBSan;
- TSan;
- P2 as dev-host regression only;
- all runs retained in order.

Add:

- X24 / negatives for repeated connection flaps and stale FINAL/current SYNC ordering;
- X25 / negative for old-socket fencing before same-member retirement.

Also strengthen X3/X4 assertions so they check the actual accounting equation, not only eventual zero:

- before and after each exact current SYNC;
- before and after FINAL;
- after a newer exact SYNC, an older control frame cannot increase `count_`.

---

# 7. Implementation authorization status

**NOT YET.**

Rev2 is much stronger than rev1, but the FINAL/current reconciliation rule is central to the no-leak/no-double-end proof and is currently inconsistent.

Do not write the D.2 implementation until rev3 makes the connection state machine exact.

The rev3 scope should be narrow:

1. split HelloAck identity from accounting-ready BOUND state;
2. require exact current SYNC before counted DATA after a connection/reconnection;
3. define FINAL semantics without adding lost `pushed - R`;
4. define stale control-frame ordering/supersession;
5. actively fence old same-member sockets before retirement on silence;
6. align blocked-primary timing and X19;
7. add X24/X25 and their negatives.

Do **not** reopen the already-corrected rev2 architecture.

Do not start S6, R640, CP-1A, comparators, B2 begin-snapshot, or S5.

---

# Final ruling

**PR #92: DESIGN REV2 SUBSTANTIALLY APPROVED WITH TWO CORRECTNESS BLOCKERS.**  
**D.2 IMPLEMENTATION: HOLD.**  
**NEXT ACTION: D.2 DESIGN REV3 ONLY.**

Rev2 successfully fixes the PR #91 architecture problems. The remaining work is now narrow and protocol-specific rather than architectural.

Standing SNC rules remain:

> Always verify, never assume.  
> Fix confirmed defects on site.  
> Correctness before performance.  
> Structural bounds must remain structural under repeated failure and churn.  
> A reconciliation protocol must be correct for reordered lifecycle events, not merely the happy reconnect path.
