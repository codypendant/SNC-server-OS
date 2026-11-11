SNC Server OS — PR #95 D.2 Implementation Independent Review

(Filed verbatim from cody's message in the project thread, 2026-11-10 22:25 UTC. HTML entities decoded.)

PR: #95 — "D.2: cross-node exchange accounting (for independent review)"
Reviewed head: "1b25846a94dd858959ff020567066d4bbb73c7f0"

Decision

PR #95: SUBSTANTIALLY CORRECT, CHANGES REQUIRED.

D.2: NOT YET COMPLETE.

Do not merge PR #95 yet and do not advance to S6, R640, CP-1A, comparators, or any later milestone.

The implementation follows the approved rev4 architecture closely, the commit decomposition is generally good, and the retained evidence is strong. The accounting barrier, exact-current SYNC, binding state machine, quarantine, retirement fencing, per-member accounting, drop-to-resume, persistent generation, and adversarial testing are materially implemented.

However, independent review found three items that must be resolved before D.2 can be approved.

Fix them in PR #95, preserve the existing evidence/history, add targeted regressions/negative mutations, then rerun the final validation gate.

---

1. BLOCKER — "--links-generation-file" has no inter-process lock

Finding

Approved rev4 section 2.1 explicitly says that an engine with no data directory using:

"--links-generation-file <path>"

must handle that file:

«under its own "flock"»

The current implementation does not.

"LinksGeneration" has an object-local "std::mutex", but that mutex protects only calls through one "LinksGeneration" object inside one process.

"LinksGeneration::open_file()" does not acquire an inter-process lock.

That means this sequence is possible:

1. process A opens generation file containing 0;
2. process B opens the same generation file containing 0;
3. A caches "value_ = 0";
4. B caches "value_ = 0";
5. A calls "next()" and durably writes 1;
6. B calls "next()" from its cached value and also durably writes 1.

Two distinct links instances have now legitimately allocated the same incarnation.

That directly violates the rev4 requirement that incarnation generation be strictly increasing and non-reusable.

The existing X29 does not test this case. It exercises the persistent data-directory path and the test-local allocator, not two independent users of an explicit generation file.

Required correction

Add a lifetime inter-process lock for an explicit "--links-generation-file".

Do not lock the generation inode and then replace that inode underneath the lock via "rename"; that can make the locking model misleading.

Use a stable lock object, for example:

"<generation-file>.lock"

or another stable lock path with:

"flock(LOCK_EX | LOCK_NB)"

The "LinksGeneration" object should retain the lock fd for its lifetime.

If another process already owns it, engine startup must fail closed with a clear message.

The persistent data-directory path may continue relying on the already-held engine data-directory lock if that ownership relationship is explicit and documented.

Required test

Add a deterministic regression, e.g. X33 — explicit generation-file exclusivity:

1. open allocator/engine A on one explicit generation path;
2. while A remains alive, attempt allocator/engine B on the same path;
3. B must refuse;
4. destroy A;
5. B may now open;
6. its next generation must be greater than the last generation A exposed.

Prefer a real process test as well as the small allocator-level test if practical.

Add a negative mutation that removes/bypasses the flock. The test must catch it deterministically.

---

2. BLOCKER — blocked-state transition retry is asymmetric

Finding

The implementation correctly fixed the defect where the blocked=true heartbeat could be delayed indefinitely under steady traffic.

But the retry logic only handles one direction of the state transition.

Current logic is effectively:

if (idle_for_T_hb || (blocked_ && !o.blocked_sent))
    push_heartbeat(...);

That retries:

"false -> true"

but not:

"true -> false".

Consider:

1. peer has already received "blocked=1";
2. its "blocked_since_" is therefore set;
3. local primary becomes unblocked;
4. "set_blocked(false)" attempts its immediate HEARTBEAT;
5. "Out::buf" has no room, so "push_heartbeat()" returns without writing it;
6. "o.blocked_sent" remains "true";
7. steady DATA/END traffic continues;
8. "last_push_ns" keeps moving forward;
9. link never becomes idle for "T_hb";
10. retry expression "(blocked_ && !blocked_sent)" is false because "blocked_ == false".

Therefore the peer can retain "blocked_since_" indefinitely despite the sender being unblocked.

That can prevent binding and can cause unnecessary quarantine/recovery behavior.

The approved design is symmetric:

«A change of the bit sends a HEARTBEAT at once.»

The peer clears "blocked_since_" only after receiving a HEARTBEAT without the blocked bit, or retirement.

Required correction

Retry whenever the state carried by the connection differs from current state:

blocked_ != o.blocked_sent

or use an equivalent explicit pending-state mechanism.

The invariant should be:

«For every UP connection, if the last transmitted blocked state differs from the current local blocked state, a HEARTBEAT remains pending until it is appended to that connection's FIFO.»

This applies to both edges:

- false -> true
- true -> false

and must remain subject to the rev4 FIFO accounting barrier.

Required test

Add an adversarial test, e.g. X34 — blocked clear under a full Out queue:

1. establish a bound link;
2. enter blocked state and prove peer observes "blocked=1";
3. make the sender's Out buffer unable to accept another frame;
4. clear blocked state while the buffer is full;
5. restore room;
6. keep ordinary traffic continuously flowing so the connection never gets an idle-heartbeat opportunity;
7. prove the sender promptly appends the clearing heartbeat;
8. prove the peer clears "blocked_since_";
9. prove normal binding/gating behavior resumes without waiting for retirement or "T_release".

Add a negative mutation restoring the current one-sided retry condition. X34 must fail deterministically.

---

3. BLOCKER — END wire encoding disagrees with the approved rev4 wire specification

Finding

The approved rev4 wire table says:

END:
    op        = n >= 1
    source.hi = target incarnation

The implementation instead sends:

f.source.hi = e.end_owed;   // n
f.target.lo = e.inc;        // target incarnation

and receives it as:

if (fr.target.lo != opt_.incarnation) ...
const u64 n = fr.source.hi;

So the implementation and the approved wire specification describe two different protocols.

The implementation is internally self-consistent, which is why the existing tests pass. But D.2 wire v2 is explicitly versioned, and its specification must be authoritative.

This cannot be left as undocumented implementation drift.

Required resolution

Choose one format deliberately and make the specification, implementation, proxy/test decoder, and comments agree exactly.

Do not silently change one side.

There is a legitimate reason to consider retaining the implementation layout: "end_owed" is represented internally as "u64", while "Frame::op" is only "u32".

If the implementation layout is intentionally preferable, document this as an implementation-discovered correction to rev4 and amend section 2.2 before approval:

END:
    source.hi = n
    target.lo = target incarnation

Then verify every relevant proof statement and receiver check uses that layout.

Alternatively, if the approved rev4 layout is the intended protocol, change the implementation to match it and explicitly establish why a u32 "n" is structurally sufficient.

Do not make that assumption implicitly.

Required test

Add a direct wire-conformance test that constructs and decodes an END frame and checks every field byte-for-byte against the documented v2 layout.

The point is to prevent a sender, receiver, fault proxy, or future independent implementation from silently choosing different field mappings while all same-build tests continue to pass.

---

4. REQUIRED TEST CLOSURE — ND10 and ND24 remain unobserved mutations

The retained negative-test evidence is useful and candid.

The final negative run reports that ND10 and ND24 mutate branches that the current Account workload cannot reach.

That explanation is plausible:

- stale/moved work is eliminated before the ND10 adoption path;
- moved tagged replies become stale before the ND24 handoff path.

However, both branches remain in generic runtime code as fail-closed defenses.

We should not call mutation coverage complete merely because the current application type cannot exercise them.

Choose one of two approaches:

Option A — prove and remove dead branches

If the branches are structurally unreachable for all legal SNC operation types, document the proof and remove/simplify them rather than retaining untestable correctness branches.

Option B — exercise them

If another legal operation shape can reach them, add a minimal test type/harness that reaches the branch and make ND10/ND24 fail deterministically.

Do not weaken the mutation gate by simply labeling them unreachable without deciding whether that is an Account-workload property or a runtime invariant.

This item does not by itself invalidate the D.2 architecture, but it must be closed before final D.2 signoff.

---

What the review accepts

Do not reopen the following without new contradictory evidence.

The following implementation directions are accepted:

- Hello v2 / HelloAck identity;
- IDENTIFIED + exact-current-SYNC -> BOUND;
- no counted DATA before BOUND;
- current-connection-only reconciliation;
- no FINAL;
- FIFO SYNC accounting barrier;
- "count = k + pushed - R";
- stale SYNC rejection;
- per-member entries;
- per-entry END debt;
- retirement fencing before state destruction;
- quarantine with one counter plus gating state;
- non-increasing quarantined debt;
- not-bound quarantine;
- drop-to-resume rather than adoption;
- blocked-primary link withdrawal;
- member steering;
- persistent incarnation generation for the ordinary data-directory path;
- fail-closed corruption/missing-generation behavior;
- "--links-generation-min";
- X30 forged-control-frame defense;
- X31 retirement purge coverage;
- X32 persistent-heartbeat evidence;
- gauge-publish pacing correction found under TSan.

The recorded failures and their root-cause history should remain in the evidence tree.

Do not rewrite history to make the implementation appear to have passed on its first attempt.

---

Validation after the fixes

After fixing items 1-4, run the new targeted tests first.

Then rerun the complete final gate on the final product head:

- Release serial suite;
- ASan + UBSan serial suite;
- TSan serial suite;
- complete "test_xnode", including all existing X tests plus the new cases;
- process xnode tests;
- F8 with accounting on;
- F8 with accounting off;
- complete negative mutation campaign including the new mutations;
- P2 dev-VM regression after correctness is green.

Retain every new attempt in chronological order.

For the final report explicitly show:

- final commit SHA;
- Release result;
- ASan+UBSan result;
- TSan result;
- xnode positive count;
- negative mutations caught / total;
- zero "xnode_end_underflow";
- zero unexpected "held_underflow";
- zero "debt_increases";
- X17 structural bound throughout churn;
- X28 accounting barrier evidence;
- X29 incarnation evidence;
- explicit generation-file exclusion test;
- blocked set and clear delivery under buffer pressure;
- final END v2 field layout.

---

Scope remains frozen

Still forbidden:

- S5;
- S6;
- R640 campaign;
- CP-1A rerun;
- comparator work;
- B2 begin-snapshot work;
- Agent-SNC implementation;
- Rust rewrite;
- DPDK/VPP.

Fix PR #95 on site and return the updated PR for independent review.

Do not create a new architectural revision unless one of these fixes actually changes a D.2 invariant. These findings are implementation/protocol-closure work, not a reason to reopen the already-approved architecture.

Final ruling

PR #95: CHANGES REQUIRED.

D.2 architecture: remains approved.

D.2 implementation: NOT YET APPROVED.

The implementation is close, but the final gate is specifically intended to catch exactly these edge cases.

Standing rules remain:

Always verify, never assume.

Fix confirmed defects on site.

Correctness before performance.

Protocol documentation and wire bytes must describe the same protocol.

A state-transition notification that can be delayed by backpressure must remain pending in both directions.

A persistent distributed generation allocator must be exclusive across every process that can allocate from it.
