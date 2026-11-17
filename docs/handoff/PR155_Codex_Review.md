# Codex review of PR #155 (relayed by cody, 2026-11-17 23:15 UTC)

Transcribed from two screenshots that cody pasted into the project. PR #155 is approved for merge as documentation,
and no freeze is authorized. The numbered recommendations are Codex's proposals for the owner's approval, not
decisions. The last line was partly hidden behind the app's reply box; the words that can be read are given as they
appear.

---

PR #155 — Protocol Acceptance Review

PR #155 — R2′ and R9 Protocol Acceptance — Approved for merge

**Decision: APPROVE PR #155 as documentation. Do not authorize the final architectural freeze yet.**

I reviewed the acceptance matrix, its classification of evidence from PRs #151–154, and the corrections to the
earlier verification claims.

Review findings

The document appropriately distinguishes bounded verification from architectural assumptions.

| Component | Assessment |
|---|---|
| R2′ verified-survivor replication | Ready to advance toward conditional freeze |
| R6.3′ reconciliation safety | Supported, with metadata-atomicity conditions |
| S2′ promotion quiescence | Supported by counterexample testing |
| Degraded-primary self-repromotion | Bounded verification passed; D-02 dependency remains |
| R9-A through R9-E | Candidate conditional freeze set |
| R9-F standby read visibility | Not verified; keep separate |
| D-02 WAL durability boundary | HOLD |

Important architectural condition

The proposed reconciliation rules assume **one atomic metadata authority per node** for term persistence, truncation
intents, and related state.

This matters for SNC because the eventual Rust architecture will use multiple shards and workers.

I recommend retaining the logical node-wide atomicity requirement while allowing the physical storage implementation
to be decided under D-06. A per-shard design must demonstrate equivalent atomicity before it can satisfy the same
contract.

What I recommend next

We should avoid another broad formal-verification cycle.

I recommend:

1. **Conditionally freeze R2′**, excluding self-repromotion's unresolved silent-loss guarantees and any clauses still
   classified TEXT ONLY.
2. **Conditionally freeze R9-A through R9-E**, after defining `ack_p` precisely in R9-B and accepting the documented
   clock assumptions.
3. **Keep R9-F outside the freeze** until its read-visibility model is verified.
4. Keep D-02 on HOLD and preserve `m = 2` as the default.
5. Move next toward resolving D-06 storage-format and metadata-atomicity decisions, rather than indefinitely
   expanding the models.

These are recommendations for your approval, not decisions already recorded.

Final ve[rdict: partly hidden] ... [ex]plicit owner decision on the conditional freeze boundaries.
