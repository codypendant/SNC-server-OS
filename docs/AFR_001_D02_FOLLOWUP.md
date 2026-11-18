# AFR-001: follow-up checks on the crash design (V4, the four-slot re-review, watermark and atomicity)

Status: **analysis only, for Codex and owner review.** Asked for by Codex's review of #162
(`docs/handoff/PR162_Codex_Review.md`), items 2 and 3, and its request for a compatibility ruling.

What this document does not do:
- accept a D-06 revision or select D-02;
- resolve R-23 or O-FLOOR;
- start the model in item 4. That needs the owner's yes, and section 4 gives the scope proposed for it.

Labels:
- [INF]: an argument made here;
- [FACT-SPEC]: read in the frozen protocol text or the TLA+ models in this repository;
- [MC]: an invariant that a model checked, within that model's bounds.

## 1. V4: can reconciliation truncate below the divergence point?

Checked against the R6 text (`docs/AFR_001_SAFETY_ANALYSIS.md` R6.0-R6.6), R6.3′ and S2′
(`docs/AFR_001_PROMOTION_DURING_RECON.md` 2.1-2.2), G-15, and the `Answer` step of `spec/tla/Log.tla`,
`PromoteRecon.tla` and `ReconBlockers.tla`. Two people worked it out separately, the author and an independent
reviewer (Opus), and reached the same result.

**Q1. Can a truncation remove an entry that agrees with the primary?**
- **Not against the primary whose bound reply produced `L`** [INF from FACT-SPEC]. Take any entry `(s, x)` that `k`
  and the answering `j` both hold:
  - logs are term-monotone, so `s ≤ t_k`;
  - `s ∈ H_j`, so `s ≤ t*`;
  - hence `x ≤ end_j(t*)` and `x ≤ end_k(t*)`, so `L = min(...) ≥ x`.
  - Each later round keeps that common prefix.
  - Premises:
    - G-15 / R6.0, so "agrees" means "same term at that LSN";
    - term-monotone logs;
    - `j`'s log only grows while it answers;
    - R6.3′'s binding of the reply to `k`'s unchanged log at `k`'s durable term.
- **Yes, by design, against a later primary.** R6.3′ item 3 and S2′ complete a pending intent after `k` learns a newer
  term, "not merely cancelled". If the newer primary holds an entry that `k` holds above `L`, `k` deletes an entry
  that agrees with its current primary.
  - The independent reviewer's trace needs 4 nodes and an operator promotion inside `S` with READY waived. Under
    automatic promotion only, it does not arise.
  - It is safe because that entry is unacknowledged: while the intent is pending, `k` counts toward no
    acknowledgement (R6.3′ item 3). G-15 permits removing it.

**Q2. Can the record that later sits at `x > L` carry the same term as the one removed there?**
- **Within one reconciliation, no** [INF]. The removed entries have terms either absent from `H_j` or `≤ t*`, while
  `j`'s entries there, and its later writes, have other terms.
- **Across failovers, yes, but only as the identical entry.** The same `(s, x)` can come back from a later primary
  that holds it. HistoryUnique (G-15) forbids the same `(term, LSN)` with different content.

**What is model-checked and what is not** [MC; bounds in `spec/tla/RESULTS.md`]:
- **Checked within bounds:** HistoryUnique, LogMatching, PositionedPrefix, EntryUnique and AckedPreserved. The bounds
  are 3 nodes and small MaxTerm and MaxIds. One run used 4 nodes, and the committed Log.tla configuration did not
  complete.
- **Matches the text:** the models' `Answer` step matches R6.2, apart from no floor and no compaction, and reply
  binding by term (or by last term and end) rather than by incarnation and request number.
- **Not checked by any invariant:** "never below divergence" and "different term at `x`". Over-truncating an
  unacknowledged agreeing entry violates none of them. Term monotonicity is not stated as an invariant either.

**Consequence for the crash design.**
- Section 3(e) of `docs/AFR_001_D02_CRASH_DESIGN.md` claimed that FL-chain always refuses a revived segment whose
  predecessor is the new log's last record. **That claim was too strong**: a later primary can bring back the
  identical entry, and then the terms match.
- 3(e) is reworded in this PR. Its safety now rests on:
  - the watermark: every acknowledged record is at or below `W`, and when FL-chain passes, `W ≤ M − 1`;
  - G-15: revived records are real historical entries;
  - re-reconciliation on rejoin.
- That argument is [INF]. A model that has silent loss and revival would check it (section 4). **V4 itself is
  answered. The safety of 3(e) is still an inference.**

## 2. Independent re-review of the corrected four-slot rules

**Reviewer:** one fresh reviewer (Opus) who had not seen the design, told to break section 2 by hand with explicit
four-slot traces, and not to write any checker.

| # | Severity | Finding | Outcome |
|---|---|---|---|
| B1 | BLOCKER | A misdirected or zeroing write longer than one slot. Four 8 KiB slots in two adjacent 16 KiB files form 32 KiB in a row, and any three adjacent slots contain a whole pair. One 24 KiB misdirected WAL write then leaves only `G − 1`: a silent rollback | **fixed in the design:** slots at offsets 0 and `M`, with `M` at least 8 KiB plus the largest write the node issues, and the gap never written. Recorded as an assumption about write size |
| B2 | MAJOR | A crash during init leaves a missing or zero-length file, and rule 3 refused it: a refusal from a crash alone | **fixed:** no valid slot means init is incomplete and restarts. Each file is fsynced after sizing |
| B3 | MINOR, UNRESOLVED | A write still in flight from a dead process could land after the next boot's write of the same generation, producing a tie with no media fault | **partly fixed:** a failed write reaps in-flight writes before exit. A process killed from outside stays open (V8) |
| m1-m4 | MINOR | Rule 4's wording when the lone copy sits in the wrong slot; other writers during recovery; the invariant as stated is not literally kept after a fault; D-06 4.1 still describes A/B | fixed: rule 4 reworded, `meta_mu` with no other writer until the boot write, invariant restated. D-06 4.1 stays as is until a revision is accepted |

**What survived, by explicit trace:**
- rule 4's duplicate torn again and again;
- the boot write torn on every boot;
- a fault on rule 4's duplicate (lost, or misdirected onto either pair);
- a fault on the boot write;
- a copy write lost several generations earlier;
- "highest valid" picking a generation that was never fully written;
- the tie rule under power loss alone;
- init with generation 1 only partly written.

**Assessment.** The re-review found one new BLOCKER and one MAJOR, both now fixed in the text, and the fixes were
not themselves re-reviewed. Each round of hand review has found something new:
- the first round found the rule-4 counterexample;
- this round found slot adjacency and init.

That pattern is the uncertainty Codex's item 4 is about.

## 3. The watermark outside the node record, against the node-wide atomicity condition

**The condition**, quoted: "The durable term, the term history, every stream's truncation intents and the incomplete
flag form one logical, atomic metadata authority per node (R6.3′ item 2). D-06 chooses the physical layout. A
per-shard layout satisfies the contract only if it shows equivalent atomicity."
(`docs/handoff/PR155_Owner_Decision.md`)

**What the condition protects** [FACT-SPEC]. R6.3′ item 2 makes "the check and the intent write … one step,
serialized against R4's term write, positioning, checkpoint publication and S2′". The condition exists so that no
crash can leave a mix of these four values that no single step produced, for example:
- an intent from one term next to a durable term from another;
- an incomplete flag that disagrees with the intents;
- a term history that disagrees with the intents.

**Precedent: the authority is already logical, not physical.** The owner accepted a term history derived from the
per-record terms in the WAL (`docs/handoff/PR157_Owner_Decision.md`). D-06 4.2 states the test it then applied:
"every `H_s` that recovery can see is a function of one record copy and the WAL under that copy's intents". The floor
lives in the manifest that the record names. So the authority already spans the record, the WAL and the manifest,
and it stays one authority because each of those is read **through** a single record copy.

**The test this document proposes for any durable value outside the record [INF]:**
1. It is not one of the four values, and no recovery decision reads any of the four values from it.
2. Recovery reads it only in a state that one record copy selects.
3. It changes only in steps that the record orders.
4. If it disagrees with the record and the WAL, the result is a refusal, never a different state.

**The watermark against that test [INF]:**

| Test | The watermark (`docs/AFR_001_D02_CRASH_DESIGN.md` section 4) |
|---|---|
| 1. Not one of the four values | It holds an LSN, a term, a CRC and a nonce. It is not the durable term, not `H` (it never adds to or removes from `H`, and recovery derives `H` from the WAL without it), not an intent, and not the incomplete flag |
| 2. Read only in a record-selected state | Recovery reads it only for a stream whose slot is NONE, in the record copy that recovery chose |
| 3. Changed only in record-ordered steps | Appends raise it only in that state, and only after the data is durable. It is lowered only at completion step 2.7, which runs while the record says PENDING. A reseed resets it while the record says RESEEDING. Each lowering is durable before the clear that ends the window |
| 4. Disagreement refuses | Every check in sections 4 and 5 of the crash design either passes or refuses. None truncates, rebuilds or picks another state |

**Conclusion [INF].** By this test the watermark is a validity check on the WAL, in the same class as a record's CRC.
It is not a fifth member of the authority. It can stop recovery, but it cannot make recovery reach a state that one
record copy and the WAL under its intents would not give.

**What the ruling would cover.** The owner rules on two points:
1. whether this test is an acceptable reading of "one logical, atomic metadata authority";
2. whether the watermark passes it.

Codex asked for an explicit ruling, so this document records no decision. This section was not independently reviewed.

**If the ruling is no**, there are two ways out, both with known costs:
- **Put the watermark in the record.** That means a record write on every flush. D-06 4.1 rules that out, because no
  record writer runs per operation, and the cost would be two mirrored 8 KiB syncs per flush.
- **Write the record and the watermark as one atomic unit.** Neither the file system nor the device offers that
  across two locations without a journal, and a journal is a new mechanism [INF].

## 4. A model, if the checks above leave doubt (proposed scope; NOT started)

**The checks above do not resolve the crash-consistency uncertainty:**
- each round of hand review of section 2 has found a new break;
- V4's consequence moved 3(e)'s safety onto an unchecked argument;
- no model has silent loss.

By Codex's item 4, that is the condition for **one small, preregistered model**. It needs cody's explicit yes. The
proposed scope follows; nothing has been written or run.

**One TLA+ module, `spec/tla/MetaWatermark.tla`, with two parts sharing one fault budget:**

1. **Record generations.**
   - State: four slots (two per file), each holding nothing, torn, or a generation with an identity tag.
   - Actions:
     - a runtime write of `g + 1` (both copies in flight, then synced, then acted on);
     - crash, which tears any subset of in-flight writes;
     - recovery: read, init-incomplete, rule 4, boot write, act;
     - one media fault: lose, tear, zero or misdirect one slot write, or misdirect a foreign write onto one slot.
2. **Watermark and truncation, for one stream.**
   - The WAL is a short sequence of `(term, lsn)` records in segments, with the void rule by name.
   - Actions:
     - appends with the data, then watermark, then acknowledgement order;
     - a truncation intent, completion steps 2.1-2.7 with the drain, the clear;
     - a lost unlink, or a lost directory entry for `L + 1`, as the fault option;
     - recovery's checks from sections 4-5 of the design.
   - Terms and replacement entries come from a fixed history that obeys G-15, so that identical entries can return
     (section 1).

**Invariants:**
- **NoRollback:** recovery never adopts a generation below the newest acted-on one, unless it refuses.
- **NoSilentLoss:** every acknowledged `(term, lsn)` is in the recovered log, or recovery refuses.
- **CrashOnlyNoRefusal:** with the fault budget unspent, recovery never refuses.
- **NoTieWithoutFault:** a tie never arises from crashes alone.

**Mutations, each expected to FAIL:**
- no rule 4;
- the boot write before rule 4;
- adjacent slots (a fault may hit two);
- K5's fixed A-then-B order;
- D-06's original A/B;
- the watermark lowered after the clear;
- no drain in 2.1;
- no `L + 1` nonce check;
- acknowledgement before the watermark.

**Bounds:**
- MaxGen 6, MaxCrash 3, one fault;
- MaxLsn 4, one truncation, two terms in the fixed history.

**Discipline (as for Completion.tla):**
- expectations committed before any counted run;
- a 2-hour cap per lane; a timeout is NOT COMPLETED;
- reachability probes for each fault kind;
- TLC state deleted after each run;
- an independent break-it review of the model's fidelity to the design.

**Out of scope:**
- O-FLOOR and reclaim;
- concurrency between streams;
- the wire;
- performance;
- process-kill I/O (V8);
- the item-8 completion internals already checked by Completion.tla.
