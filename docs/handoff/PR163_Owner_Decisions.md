# Owner decisions after PR #163 (cody, 2026-11-18)

This is the owner's decision record, in the same form as `PR148_Owner_Decisions.md`.

## The decisions

cody wrote the following in the project. Both messages are quoted verbatim.

> Yes on all three.

That was at 18:50:58 UTC. Seven seconds later:

> Let me let Codex look at it first, too.

Codex's updated review of #163 (`PR163_Codex_Review.md`) arrived through cody at 18:53 UTC. With it, the first
message stands.

The three items answered the coordinator's project-chat post. They are as the coordinator relayed them [the post's
wording is not reproduced here]:
1. Write down "one data write in flight per stream" as a rule.
2. Treat gap 2 (a revival after a lost `L + 1` binding) as harmful, and keep the check in every watermark write.
3. One more small preregistered model run to test that fix, under the same limits, with no R640.

## What is recorded

| # | Decision | Status |
|---|---|---|
| OD-1 | **At most one data flush in flight per WAL stream.** This is a design rule. It matches the current code (`src/core/storage.cpp:643`). | **Owner decision.** Codex: YES |
| OD-2 | **Persistent truncation binding.** Every watermark write carries `(L + 1, segment nonce)` until the next truncation, and recovery checks it every time. | **Owner decision** on the direction. Codex: YES, "still requires verification" |
| OD-3 | **Revision 3 of `MetaWatermark.tla`**, scoped by Codex: the persistent binding and the serialization assumption. It keeps preregistration, negative mutations and independent review, with a 2-hour cap per lane, no R640, and one PR. | **Authorized** |
| OD-4 | **The watermark is outside the node-wide metadata authority, CONDITIONALLY ACCEPTED.** It is an independently persisted durability validator. It is not an authority for the node's term, truncation intent or incomplete state. Condition: any disagreement makes recovery refuse, and is never read as another account of the authoritative state. Whether the combined crash behaviour is safe still needs verification. | **Owner decision.** Codex: CONDITIONALLY YES |

OD-4 was decided separately. The coordinator asked cody in the project chat [paraphrased by the coordinator]: "Codex's
third item isn't one of the three you approved: it rules that the watermark may live outside the node record, as a
check only. If it ever disagrees with the record, recovery refuses to start. Do you accept that, with that condition?
I recommend yes." cody answered, verbatim, at 18:53:53 UTC:

> Yes.

## What stays open

D-02, R-23 and O-FLOOR stay open. No D-06 revision is accepted as final. Nothing here authorizes C++, Rust, R640 or
X-2 work.
