# PR #95 D.2 implementation: final independent review

Filed verbatim from cody's message in the project thread, 2026-11-11 13:39 UTC.

---

SNC Server OS — PR #95 Final Independent Review

PR: #95 — "D.2: cross-node exchange accounting"
Current PR head: "865493383fe8698775ce87146eb7c4cde1ae33bf"
Final validated product/test head: "2bccdbe5e3092f06c018f3d799a99428020997d6"
D.2 review-fix product code: "b77937ecbd74b075d92bf7e521b7513fa9f080c1"

Final decision

PR #95: APPROVED.

D.2: COMPLETE.

MERGE AUTHORIZED.

The first independent review's four closure items have been satisfactorily resolved, and the complete validation gate has been rerun.

Do not reopen the approved D.2 architecture without new contradictory evidence.

---

Review item 1 — explicit generation-file exclusivity

CLOSED.

"--links-generation-file" now owns a stable:

"<path>.lock"

for the allocator lifetime using:

"flock(LOCK_EX | LOCK_NB)".

The lock is acquired before reading the generation file and retained until the "LinksGeneration" object is destroyed.

This closes the duplicate-incarnation race between independent processes using the same explicit generation file.

The ordinary data-directory generation continues to rely on the engine's existing lifetime data-directory lock.

Evidence:

- X33 allocator-level exclusion;
- process x33;
- ND40 removes the exclusion and X33 catches it.

Approved.

---

Review item 2 — blocked-state transition delivery

CLOSED.

The state notification is now symmetric.

Each Out connection records the last blocked state it actually carried as:

- "-1": none yet;
- "0": unblocked;
- "1": blocked.

A heartbeat remains owed whenever:

"blocked_sent != current blocked state"

so both transitions remain pending through backpressure:

- false -> true;
- true -> false.

This fixes the case where the clearing heartbeat could previously disappear under continuous traffic after an initial full-buffer failure.

Evidence:

- X34 fills the Out path, changes blocked state, restores room under steady traffic, and verifies the clear reaches the peer;
- ND39 restores the one-sided retry and X34 catches it.

Approved.

---

Review item 3 — END wire protocol

CLOSED.

The specification and implementation now define one END layout.

Wire v2 END:

- "source.hi" / bytes 0..7 = "n", u64;
- "target.lo" / bytes 24..31 = target incarnation;
- byte 41 = kind "END";
- remaining END-specific bytes zero.

The implementation uses shared encoder/decoder helpers, and malformed "n = 0" END frames are ignored.

X35 checks:

1. the encoder byte-for-byte;
2. a real END captured from the wire;
3. rejection of the superseded rev4 mapping;
4. rejection of "n = 0".

ND41 restores the former mapping and X35 catches it.

Approved.

---

Review item 4 — ND10 / ND24 runtime invariants

CLOSED.

The runtime invariant treatment is acceptable.

Design section 2.9.1 now states the premises and proofs explicitly.

I1

An untagged item reaching a node-link send path must belong to that shard's own exchange.

Cross-shard cases that would violate this are eliminated by source/target placement-epoch staleness before the node-link path.

"xnode_drops_to_resume" and "xnode_uncounted" therefore remain must-stay-zero invariant counters.

I2

A tagged item must never be handed to another shard of the same node.

The runtime placement/staleness rules prevent that transition for every legal reply-only follow-up shape.

Most importantly, X12c now exercises the previously missing ordering case:

- tagged arrival routed to a shard;
- execution deliberately paused;
- source slot moves;
- item remains routed but unrun;
- execution resumes.

The runtime execution-time staleness check must drop it before it can reach the in-node handoff.

ND24 removes that premise and X12c catches the violation through the I2 invariant.

ND10 similarly mutates the source-side staleness premise and is caught.

This is now evidence-backed rather than merely a written unreachability claim.

Approved.

---

Final validation gate

At "2bccdbe":

- Release serial suite: 47 / 47 pass
- ASan + UBSan serial suite: 47 / 47 pass
- TSan serial suite: 47 / 47 pass, no TSan report
- complete "test_xnode": 33 / 33 cases
- process tests x6, x7, x7b, x33: pass
- x9 mixed-version test against D.1 "33f7c6d": pass
- negatives: 42 / 42 caught
- F8 accounting on/off: pass in Release, ASan and TSan
- P2: accounting-on approximately 1% below accounting-off, inside run-to-run spread; no performance conclusion beyond regression suitability

Required invariant counters remain zero in the successful X-node tests:

- "xnode_end_underflow"
- "xnode_held_underflow"
- "xnode_debt_increases"
- "xnode_uncounted"
- "xnode_drops_to_resume"
- "xnode_handoff_drops"

Key structural evidence:

- X17: "live_s < X + max(E,Rr)" at every sample through 20 churn cycles;
- physical-work structural bound held at every sample;
- X28 proves the FIFO SYNC accounting barrier;
- X29 proves incarnation monotonicity independent of wall clock and across injected generation-update crashes;
- X33 proves explicit-file allocator exclusivity;
- X34 proves blocked set/clear delivery through backpressure;
- X35 proves END byte layout.

The product code does not change after "b77937e"; commits through "2bccdbe" refine tests/docs, and current PR head "8654933" adds the final evidence/coordination records.

---

Retained observations

Do not delete or rewrite the earlier failed runs.

Two transient failures remain without a proven root cause:

1. one Paxos c2b deadline failure in an intermediate Release run;
2. one F8 node-links-up timeout observed once during repeated testing.

Neither reproduced in the final gate, neither required a D.2 product change, and there is currently no evidence that either represents a D.2 safety failure.

Carry them forward as observations. If either reproduces during campaign preparation, investigate it on site rather than dismissing it as noise.

The low-disk b3 failure is separately explained by the host running low on disk and reproducing on pre-D.2 main; that is not a D.2 finding.

---

Merge / record actions

1. File this final review as the PR #95 D.2 implementation final review.
2. Update "docs/STATUS.md":
   - PR #95 APPROVED;
   - D.2 COMPLETE;
   - record the final evidence directory and validated SHA.
3. Add the concise AGENTS handoff.
4. Merge PR #95.
5. Do not mix the next engineering slice into PR #95.

---

Immediate post-merge action: refresh the R640 hardware baseline

The R640 hardware configuration has changed since campaign manifest d.

The machine previously had five of six memory channels populated per socket.

Cody has now restored RAM so all six memory channels are populated.

Therefore:

"bench/manifests/r640-main-20261106d.txt" is no longer a valid description of the machine for new campaign evidence.

Do not run a new CP-1A pilot, S6 evidence campaign, comparator, or freeze the experimental baseline against manifest d.

After PR #95 is merged:

1. update the R640 checkout to the merged "main";
2. leave the newly installed RAM in its intended campaign configuration;
3. set the approved runtime prerequisites:
   - CPU governor "performance";
   - "kernel.numa_balancing=0";
4. capture a new complete R640 manifest with "bench/manifest.sh";
5. verify "dmidecode"/inventory shows all six channels populated on both sockets;
6. record total RAM, DIMM slots, speed/ranks, NUMA memory totals, CPU topology, SSD, NIC, governor, kernel settings, IRQ/RSS and all normal manifest evidence;
7. update "docs/R640_TOPOLOGY.md" to supersede manifest d for future campaign runs;
8. do not reinterpret or overwrite the old CP-1A results—the old results remain valid historical evidence for the old machine configuration;
9. return the new manifest/topology update for review before running new R640 experimental evidence.

The hardware change creates a new experimental machine baseline, not a reason to rerun old evidence silently.

---

Authorization boundary

The D.2 prerequisite for S6 is now satisfied.

However, do not start S6 or a new R640 campaign in the same change.

First:

merge #95 -> record D.2 COMPLETE -> recapture/review the changed R640 hardware manifest.

Then return for the next explicit campaign/S6 authorization with the actual current machine configuration.

No comparator results before the baseline criteria permit them.

No Agent-SNC, Rust rewrite, DPDK/VPP, or unrelated platform work.

Final ruling

PR #95: APPROVED.

D.2: COMPLETE.

Merge authorized.

Standing rules:

Always verify, never assume.

Fix confirmed defects on site.

Correctness before performance.

Keep failed evidence; do not clean history to make results look better.

A hardware change invalidates the old machine manifest for future experimental evidence.
