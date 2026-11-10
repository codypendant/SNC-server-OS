# SNC Server OS — PR #93 D.2 Design Rev3 Review Handoff

**PR:** #93 — `D.2 design rev3: exact binding SYNC, no FINAL, fencing before retirement`  
**Reviewed head:** `ca1caffa3783a2669f23b02779d5533a126f4617`  
**Merged as:** `45340a34bbc0a4c409f6da87698696aaadadd739`

## Decision

**PR #93: REV3 CORE PROTOCOL APPROVED, IMPLEMENTATION HOLD ON TWO NARROW CORRECTNESS ITEMS.**

Rev3 successfully closes the PR #92 blockers:

- HelloAck now proves identity only.
- Counted DATA cannot flow until an exact current-connection SYNC has reconciled accounting.
- FINAL SYNC is removed.
- Old connection history is removed.
- Stale SYNCs are ignored.
- Silent same-member replacement fences old sockets before retirement.
- Blocked-primary timing is now explicit and testable.
- X24/X25 and the stronger negative program address the specific rev2 review failures.

The architecture is now substantially stable. Do **not** reopen the rev1/rev2 decisions.

However, two remaining protocol details are not safe enough to implement as currently written:

1. the binding SYNC is allowed to overtake already-accounted frames in the sender's user-space output queue;
2. incarnation ordering is based on `CLOCK_REALTIME` plus random low bits, which is not a monotonic incarnation sequence.

Both are narrow. Fix them in **design rev4 only**, then return for review.

---

# 1. BLOCKER — the SYNC stream-position rule can double-decrement accounting

Rev3 section 2.5 says:

> The SYNC goes ahead of p's frames that are still queued in user space, never ahead of bytes already handed to the socket.

It then claims:

> every END and counted reply that k excludes is already ahead of it in the byte stream.

Those statements are not generally compatible.

The current `NodeLinks` output implementation is a FIFO user-space byte buffer (`Out::buf`). `NodeLinks::push()` appends a 64-byte frame to that buffer. A frame can therefore already have performed its accounting transition in `Shard` while still sitting unsent in `Out::buf`.

## Concrete failure

Take an item belonging to origin s that is held at peer p.

1. `held_for_[e] == 1`.
2. p creates a counted reply to s.
3. The push rule decrements `held_for_[e]` to 0.
4. The reply DATA frame is appended to p's user-space Out buffer, but the socket cannot flush it yet.
5. p then receives a new Hello from s and owes a binding SYNC.
6. Rev3 allows that SYNC to go **ahead of frames still queued in user space**.
7. The SYNC samples `k = 0`.
8. s receives the SYNC first and sets `count = 0`.
9. The earlier counted reply is later delivered.
10. s applies the reply and decrements `count` again.

Result: underflow / double end.

The same class exists for an END frame that has already been encoded into the user-space byte stream before the SYNC.

This is not theoretical: the current node-link implementation has exactly one per-Out user-space `buf`, and `push()` appends frames before `flush()` hands them to the kernel.

The rev3 independent review correctly noticed an ordering hazard, but the stated fix — "ahead only of the user-space queue" — is still too aggressive.

## Required correction

Define a strict **accounting barrier** for SYNC.

A SYNC may not overtake any frame whose accounting effect is already reflected in the sampled state.

The simplest rule is:

> **A SYNC is appended FIFO after every frame already encoded into that Out connection's byte stream. It may preempt only work that has not yet executed its accounting transition and has not yet been encoded.**

In practice:

- bytes already in the socket/kernel stay first;
- bytes already in `NodeLinks::Out::buf` stay first;
- the SYNC is appended after them;
- it may be prioritized ahead of higher-level `emit_node_` work that has not yet been pushed/accounted;
- `k`, `rx`, and the clearing/subsuming of `end_owed_` are sampled when the SYNC is appended at that barrier.

Then the ordering proof becomes valid:

- any counted reply or END already reflected as absent from `k` is ahead of the SYNC;
- anything behind the SYNC has not yet performed the accounting transition that would remove it from `k`.

Do not implement a prepend/priority insertion into an already-accounted `Out::buf`.

## Required test

Add:

### X28 — queued accounting frames cannot be overtaken by binding SYNC

Setup:

- make p's Out socket to s temporarily non-writable so frames accumulate in the user-space Out buffer;
- create at least:
  - one counted reply already encoded in that buffer;
  - one END already encoded in that buffer;
- then cause s to reconnect so p creates a binding SYNC;
- restore output.

Pass criteria:

- the already-encoded reply/END are observed before the SYNC;
- at the exact SYNC, `count` equals the physical/accounted state;
- later frames do not cause underflow;
- `xnode_end_underflow == 0`;
- quiescence reaches all-zero counts.

Negative mutation:

### ND36

Insert the SYNC ahead of already-encoded user-space frames.

X28 must fail deterministically.

---

# 2. BLOCKER — `CLOCK_REALTIME + random` is not an ordered incarnation identifier

Rev3 changes `incarnation` so:

- high 41 bits = process start time in milliseconds from `CLOCK_REALTIME`;
- low 23 bits = random;
- peers accept only a **higher** incarnation.

The design then calls incarnations "ordered by start time".

That is not structurally true.

## Failure A — two starts in one millisecond

If two legitimate starts occur in the same millisecond:

- both have the same high 41 bits;
- ordering is decided by the 23 random bits.

The later process can randomly receive a **lower** incarnation than the earlier process.

The peer will reject the legitimate new process as stale.

This is an availability failure created by randomness in a field being used as a monotonic generation.

## Failure B — wall-clock rollback

`CLOCK_REALTIME` can move backward.

If an earlier process started while the clock was ahead and a later legitimate restart occurs after the clock is corrected backward, the later incarnation is lower.

The note says:

> the new instance is refused until it passes the old start time.

But an instance's incarnation is chosen once at construction. It does not increase while the process waits.

Therefore that process does **not** eventually "pass" the old incarnation. It remains refused until another restart happens with a sufficiently large value.

A large forward clock error followed by correction can create an arbitrarily long fail-closed outage.

## Failure C — 41-bit millisecond lifetime

41 bits of Unix milliseconds reach their limit around 2039. Even though that is not the immediate blocker, it is another reason not to make this the durable generation format.

## Required correction

Use a **monotonic per-member incarnation generation**, not wall-clock ordering.

Recommended v0.1 design:

- persist a `links_incarnation_generation` (u64) in the member's data directory;
- while holding the existing proposed data-dir exclusive lock:
  1. read the generation;
  2. checked-increment it;
  3. durably store the incremented value **before opening the node-link listener or any Out connection**;
- use that generation as the incarnation ordering key on the wire.

A random nonce may still be carried separately if desired for connection uniqueness, but randomness must not decide "newer incarnation".

Crash consistency of the generation update must be specified. A torn or ambiguous generation update should fail closed, not reuse an older generation.

For in-memory test engines, use a test-local monotonic allocator rather than `CLOCK_REALTIME`.

If restoring an older data directory can roll the generation backward, document that as a recovery exclusion or define the explicit operator recovery mechanism. Do not silently accept a lower generation.

## Required tests

Add:

### X29 — incarnation generation is monotonic without wall-clock assumptions

Cover at least:

1. two process starts forced into the same wall-clock millisecond;
2. wall clock stepped backward between legitimate restarts;
3. wall clock stepped forward and then corrected backward;
4. rapid repeated restarts.

Pass criteria:

- every legitimate new incarnation is greater than the prior one;
- delayed old Hellos are refused;
- the new member can bind without waiting for wall time to catch up;
- no entry is retired by an older incarnation.

Add a negative:

### ND37

Generate incarnation from `CLOCK_REALTIME` with random low bits as rev3 currently specifies.

X29 must fail.

---

# 3. PR #92 blockers — status after rev3

## HelloAck identity vs accounting readiness

**RESOLVED.**

Rev3 correctly separates:

- IDENTIFIED by same-socket HelloAck;
- SYNCED by exact current SYNC;
- BOUND only when both agree on the same entry.

No counted DATA flows before BOUND.

Keep this.

## FINAL SYNC arithmetic/history

**RESOLVED.**

Removing FINAL is the correct simplification.

Current-connection-only reconciliation plus no old connection history is substantially easier to prove.

Keep:

- random connection nonce for identity;
- `conn_seq` as an additional current-connection ordering discriminator;
- stale SYNC ignore/count behavior;
- X24.

## Same-member silent retirement fencing

**RESOLVED.**

Rev3 now explicitly:

- removes the old In socket from epoll;
- resets/closes it;
- resets any Out connection naming the old entry in any state;
- retires only after transport fencing;
- refuses later traffic claiming the retired incarnation.

Keep X25/ND30.

## Blocked-primary timing

**RESOLVED.**

The state machine now distinguishes:

- blocked bit delivered: deadline based on `blocked_since + T_release`;
- blocked bit not deliverable: links leave at `T_block`, then In-down quarantine applies.

X19/X19b now match the written rules.

---

# 4. Rev3 questions

## Q9 — remove FINAL

**APPROVE.**

The current-connection exact SYNC is enough if the stream barrier above is corrected.

Removing old-connection reconciliation state is a net improvement.

## Q10 — not-bound quarantine

**APPROVE.**

Without FINAL, a formerly bound entry can retain a conservative over-count if it never becomes the bound entry again.

The not-bound quarantine trigger is an appropriate bounded availability mechanism, provided:

- no counted DATA reaches the entry while not BOUND;
- quarantine resets any current Out path naming it;
- debt remains non-increasing.

Rev3 satisfies that direction.

---

# 5. Earlier PR #92 section-10 answers remain unchanged

The following remain approved:

- drop-to-resume instead of adoption;
- Invariant M as the v0.1 one-live-instance-per-member basis, subject to the monotonic incarnation correction above;
- the loose structural B2 constant;
- quarantine;
- timer ordering as hypotheses;
- In replacement per member;
- HelloAck;
- blocked-primary node-link withdrawal within D.2 scope.

Do not reopen these.

---

# 6. Test program after rev4

Retain the complete existing program:

- X1-X27;
- ND1-ND35;
- Release;
- ASan+UBSan;
- TSan;
- P2 as dev-VM regression only;
- all attempts retained in order.

Add:

- **X28 / ND36** — SYNC stream barrier with already-encoded reply/END frames;
- **X29 / ND37** — monotonic persistent incarnation generation under same-time and clock-step scenarios.

Strengthen the accounting assertions so an exact binding SYNC records enough evidence to compare:

- origin `count`;
- peer `held_for`;
- current `pushed`;
- current `rx`;

at the barrier where the SYNC is encoded/applied.

---

# 7. Implementation authorization

**HOLD for one final narrow design revision.**

The remaining changes do not alter the D.2 architecture:

- no change to origin counting;
- no change to END;
- no change to quarantine;
- no change to drop-to-resume;
- no change to B2 terms;
- no change to per-member entries;
- no change to blocked-primary handling.

Rev4 should change only:

1. the exact byte-stream position/barrier of SYNC;
2. the incarnation-generation mechanism;
3. X28/X29 and ND36/ND37;
4. any wording/proof statements affected by those two corrections.

If rev4 closes those points without reopening scope, D.2 should be ready for implementation review authorization.

Still forbidden:

- S6;
- R640 campaign;
- CP-1A rerun;
- comparator campaign;
- B2 begin-snapshot work;
- S5 unless separately justified.

---

# Final ruling

**PR #93: REV3 CORE PROTOCOL APPROVED.**  
**D.2 IMPLEMENTATION: HOLD ON TWO NARROW CORRECTNESS ITEMS.**  
**NEXT ACTION: D.2 DESIGN REV4 ONLY.**

Rev3 fixes the substantive PR #92 problems. The remaining issues are now limited to a stream-ordering barrier and a structurally monotonic incarnation identifier.

Standing SNC rules:

> Always verify, never assume.  
> Fix confirmed defects on site.  
> Correctness before performance.  
> A control snapshot may not overtake data whose accounting it already reflects.  
> Distributed generation ordering must not depend on wall-clock monotonicity.
