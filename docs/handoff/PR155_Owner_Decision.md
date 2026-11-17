# Owner decision on the conditional freeze boundaries (cody, 2026-11-17)

This is the owner's decision record, like `PR148_Owner_Decisions.md`.

## The decision

cody's message in the project, 2026-11-17 23:16 UTC (5:16 PM CT), verbatim:

> Yes.

It answered the coordinator's project-chat post. That post put Codex's five recommendations from the review of PR #155
(`docs/handoff/PR155_Codex_Review.md`) to cody as one decision, recommended yes on all five, and said a yes starts the
freeze record and the D-06 design work, docs only. The five items as posted:

1. Conditionally freeze R2′, leaving out self re-promotion's silent-loss guarantees and any text not yet reviewed
   (TEXT ONLY clauses).
2. Conditionally freeze R9-A to R9-E, once R9-B defines the acknowledged position (`ack_p`) precisely and with the
   documented clock assumptions accepted.
3. Keep R9-F out until its own model is checked.
4. Keep D-02 on hold and m = 2 as the default.
5. Go next to D-06, the storage format and metadata atomicity design. The contract stays "atomic per node"; a
   per-shard layout has to prove equivalence.

## What is frozen, exactly

"Conditionally frozen" means the text is the contract that the Rust architecture is designed against. It changes only
through a recorded revision with new evidence. It is **not** a claim that the C++ conforms, and it does **not**
authorize Rust. Those are separate gates (`PR148_Owner_Decisions.md`). The classification below is from
`docs/AFR_001_PROTOCOL_ACCEPTANCE_R2P_R9.md` (PR #155).

**R2′** (`docs/AFR_001_R2_AND_SINGLE_COPY.md`; `docs/AFR_001_PROMOTION_DURING_RECON.md` 2.1-2.2):
- rules 1-5;
- P1-P7 with P6′, except P5's clause that relies on a copy not being silently lost;
- S1-S5 with S2′;
- R6.3′ items 1-3, except the TEXT ONLY parts listed below;
- completion requirements, 2.2 items 1 and 4: the logical end is recorded before any file changes; the procedure is
  idempotent; the intent is cleared only once the rest is durable;
- 4.2: a restarted sole primary serves again only through a new witness-approved term, never at its old term.

**R9-A to R9-E** (`docs/AFR_001_SAFETY_ANALYSIS.md` R9), with `ack_p` defined in R9-B (this PR).

## Excluded from the freeze

- **4.2's silent-loss guarantees, and P5's silent-loss clause.** If p silently lost its newest flush, reconciliation
  against the re-promoted p can delete the last copy of an acknowledged write. These remain UNRESOLVED until D-02.
- **TEXT ONLY clauses**, until they are reviewed again:
  - R6.3′ item 1's group and stream identity, the incarnation persisted before the first request, and the rule that
    an in-progress checkpoint counts;
  - the answering side: j stops answering when it loses its tail, and its positioned record for k ends at k's
    restart;
  - completion item 5, the valid-end marker. Items 2-3 are the C++ realization and belong to D-06.
- **R9-F** and standby lease reads that depend on it.
- **D-02**, which stays on HOLD.

## Conditions

- **Node-wide atomicity (item 5).** The durable term, the term history, every stream's truncation intents and the
  incomplete flag form one logical, atomic metadata authority per node (R6.3′ item 2). D-06 chooses the physical
  layout. A per-shard layout satisfies the contract only if it shows equivalent atomicity.
- **Clock assumptions (item 2).** These are G-46's: a bounded rate drift ρ and a clock that counts through suspend,
  with VM pause out of contract. The R9-E formulas `d(1+ρ)` and `(1-ρ)/(1+ρ)` are accepted as argued, not checked.
- **Arguments accepted.** The ARGUED rows of the acceptance review are accepted as arguments, including:
  - rule 5, P6/P6′, R6.3′ item 2's serialized step, completion items 1 and 4;
  - R9-A for replicas still catching up.
- **Reopening.** A finding in D-02, D-06 or the consensus service (D-13) that contradicts frozen text reopens it
  through a recorded revision.
- **m = 2** stays the default (item 4). Its failover-availability cost is still to be revisited, as the owner direction
  of 2026-11-17 requires, without silently weakening it.

## Next, authorized

D-06 design (storage format and metadata atomicity), docs only.

**Not authorized:**
- C++ or Rust work;
- R640 runs;
- the X-2 harness or runs;
- D-02 acceptance;
- an R9-F freeze;
- any change to m = 2.
