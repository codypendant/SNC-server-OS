# PR #101 S6 runner + final preregistration: final review

Filed verbatim from cody's message in the project thread, 2026-11-11 18:10 UTC
(relayed because the reviewer's GitHub connector is read-only).

PR #101 merged as `63440b12288f9aae36c5309fa21b759d0c9a36fd`.

---

PROJECT CODEX HANDOFF — PR #101 FINAL REVIEW

Repository:

"codypendant/SNC-server-OS"

PR:

"#101"

Reviewed head:

"68c3fe4b4042244e719ae27f99de279e0b9c9302"

Decision

PR #101: APPROVED.

S6 runner: APPROVED.

S6 preregistration: APPROVED.

Review-1 closure: COMPLETE.

MERGE AUTHORIZED.

S6 itself is NOT GO yet.

---

Review-1 closure verification

1. Measured client exit — CLOSED

"correctness_ok()" now requires:

"client_rc == 0"

Missing, positive nonzero, or signal-style negative status fails rule 7.

This applies to:

- P/N/H open-loop cells;
- C/C0 closed-loop capacity cells.

A measured-client failure is a result, not an automatic harness rerun.

Mutation coverage includes:

- "NEW1-no-client-rc"
- "NEW9-closed-run-rc-only-open"

Both are caught.

2. Sampler catch-up burst — CLOSED

"next_deadline()" now advances to the first future deadline after an overrun.

Missed sampling periods are skipped and recorded rather than issued back-to-back.

This preserves the 100 ms nominal sampling grid without turning a delayed monitor into additional load.

Skipped deadlines are explicitly reported.

Mutation:

"NEW2-catch-up-restored"

is caught.

3. Checkpoint-capacity window — CLOSED

Checkpoint-window proof now applies to:

- P
- H
- C
- C0

"checkpoint_window()" requires at least one bounded "ckpt_seq" advance inside the measured interval.

C/C0 therefore cannot be accepted as “checkpoint capacity” unless a checkpoint actually began during their measured closed-loop interval.

No proven checkpoint inside the window:

"void = "window""

with the preregistered one-rerun policy.

Mutation:

"NEW3-no-closed-window"

is caught.

4. Storage identity — CLOSED

Pre/post machine identity now verifies the data disk resolved by the existing device resolver, not a hard-coded "/dev/sdX".

Required state:

- model "ARFX3840S5xnFTRI"
- active scheduler "[mq-deadline]"
- write cache "write back"
- FUA "1"
- filesystem UUID "3bf857c8-0cb4-4c57-b9a5-14328e70b061"
- ext4
- mount options contain:
  - "rw"
  - "relatime"
  - "stripe=2"

Fresh R640 preflight reports every one of these checks true.

Mutations caught:

- NEW4 scheduler
- NEW5 write cache
- NEW6 FUA
- NEW7 mount-options weakness
- NEW8 filesystem UUID

---

Validation accepted

Review-1 focused gate:

4/4 passed

- "test_ckpt_cli"
- "test_profile_check"
- "test_pilot"
- "test_s6"

Mutation suite:

28/28 caught

- original 19 mutations
- 9 new review-1 mutations

Fresh preflight verifies:

- manifest SHA
- kernel
- CPU count/model
- NUMA CPU maps
- memory
- governors
- "numa_balancing"
- CPU allocation
- SSD model
- scheduler
- write cache
- FUA
- filesystem UUID
- ext4
- mount options
- all three binary hashes

A real invocation before freeze correctly refuses before starting any process because:

- "runner_commit" is unset;
- "go_record" is unset.

That behavior is approved.

---

Compatibility check

The current "net_bench" → pre-S1 "389b623" server compatibility check is ACCEPTED.

No dev-VM repeat is required.

It was deliberately non-S6:

- shortened 5 s window;
- checkpoint interval 8 s;
- boot-disk storage;
- no S6 verdict;
- no performance interpretation.

It established interface/harness compatibility only.

Do not use its throughput or latency as campaign evidence.

---

Non-blocking documentation cleanup

"bench/s6.py" still contains one stale docstring phrase:

"window: no checkpoint began inside the measured window (P, H only)."

The implementation correctly covers P/H/C/C0.

This wording may be corrected to:

"P, H, C or C0"

before merge if desired.

It is not a correctness blocker.

If changed, make it a comment/docstring-only edit.

---

Merge instructions

PR #101 may now merge.

After merge, record the exact PR #101 merge SHA.

Do not run S6 yet.

Do not make any product-code, runner, test, threshold, workload, pacing, allocation, binary, or methodology change after this merge.

---

NEXT AND ONLY AUTHORIZED STEP — FREEZE CLOSURE

Create a tiny docs/freeze-only change after PR #101 merges.

It must:

1. Set in "bench/s6_frozen.json":

   "runner_commit = <exact PR #101 merge SHA>"

2. Update "docs/S6_PREREG.md" section 1.2/1.3 with that exact merge SHA.

3. File this final review in:

   "docs/handoff/PR101_S6_Runner_Final_Review.md"

4. Update "docs/STATUS.md" and "AGENTS.md" concisely:

   - PR #101 approved;
   - runner/preregistration complete;
   - exact merge SHA recorded;
   - awaiting final freeze verification / S6 GO.

5. Leave "go_record" unset/null.

The GO has not been issued yet.

Do not bypass this by pointing "go_record" at a review that does not explicitly authorize execution.

Return this docs/freeze-only change for one final independent verification.

---

No changes allowed in freeze closure

Do not modify:

- "bench/s6.py"
- "tests/test_s6.py"
- any C++ source
- binary builds
- pacing values
- target/max age
- cell definitions
- thresholds
- PASS/FAIL/VOID rules
- sampling interval
- comparator commit
- CPU allocation
- workload
- run order

The freeze closure is identity/coordination only.

---

After freeze verification

Only after the exact merge SHA and freeze are independently verified will the explicit:

S6 GO

be issued.

Until then:

DO NOT RUN S6.

Still forbidden:

- CP-1A rerun
- external comparator systems
- CRITERIA freeze
- S5/ioprio
- B2
- Agent-SNC
- Rust
- DPDK/VPP

Standing rules:

Always verify, never assume.

Fix confirmed defects on site.

No performance parameter changes after preregistration.

No campaign execution before the explicit GO.
