# AFR-001: I2-I5 for a reseeded node that is complete but not positioned (PROPOSED)

Status: **PROPOSED analysis, for Codex and owner review.** Docs only. It is the condition the owner set on Y6c-2
(`docs/handoff/PR171_Owner_Decisions.md`): "do not accept the frozen-contract revision until I2–I5 have been
explicitly re-argued for the state where a node is complete but not yet positioned. The analysis must cover
acknowledgments, leases, promotion, membership, crash/restart, and reconciliation." **R6.5's frozen text is
unchanged; nothing here is accepted.** Every claim is **[INF]**, argued, not model-checked.

Sources: I1-I5 and R1-R9 in `docs/AFR_001_SAFETY_ANALYSIS.md` (SA); R2′ in `docs/AFR_001_R2_AND_SINGLE_COPY.md`;
R6.3′, S2′ and P6′ in `docs/AFR_001_PROMOTION_DURING_RECON.md`; the reseed steps in
`docs/AFR_001_DURABILITY_CONTRACT.md` 6.5 and 6.6 (the contract) and note C.2.

## 1. The state

**U** is the state of a node `k` from RS-6's record write (flag cleared, the copied checkpoint published) until an
R6.2 round positions it or it is reseeded again, **across restarts**: a restart in U leaves `k` in U. In U, `k`:
- is not in S (it left by a chosen shrink before RS-1, R2′ rule 3) and holds no lease (RS-0 dropped them);
- holds a log copied from one primary `j`: the parts of `j`'s checkpoint, then `j`'s WAL from `start_lsn`, received
  in RS-3 to RS-5 while `k`'s durable term was `t`, `j`'s term;
- is neither incomplete nor reseeding (both words mean the durable flag, contract 6.6), and is not positioned.

**Four rules this analysis uses.** U1 and U2 are in the contract's 6.5 or the revision text; U3 and U4 are added
here as part of the same Y6c-2 text:
- **U1.** In U, `k` sends no acknowledgement report, holds and requests no lease, and is not admitted to S (the
  revision text, contract 6.6).
- **U2.** The transfer is bound to `j`'s term `t` and incarnation; a change of either sends `k` back to RS-2
  (contract 6.5). RS-6 is a compare-and-write on `t`.
- **U3 (new).** `j` serves RS-3 to RS-5 only while it is primary of `t`, as it answers R6.2 only then (R6.3′ item 1,
  answering side). After RS-5, `k` accepts no entry until R6.2 positions it.
- **U4 (new, round 4).** RS-1's record write takes a new incarnation. R6.3′ ends a primary's record that `k` is
  positioned only when `k`'s incarnation changes, and a reseed in place has no boot write; without U4 a primary that
  had positioned `k` before the reseed would still treat it as positioned (lease test, shipping).

**What U1 and U4 protect, and what they do not need to [INF, round 4].** HA_DURABLE needs every member of S. G-12
admits writes only while `|S| ≥ m = 2`, and every member of S other than `p` is positioned on `p`'s log (a survivor
by R2′ rule 2, a grown member through "caught up"). `k` is not in S. So even a wrongly counted report from `k` could
not make an acknowledged entry have fewer than two real copies: it would break section 0's "every member of A has
`e`", not G-11. U1 and U4 keep that statement true; the I4 argument for S does not depend on them. [Round 5: too strong. A
**delayed** report can still break G-11 when the primary has missed a shrink, and a stale report can let a member be
grown into S without the entry; see `docs/AFR_001_REPORT_IDENTITY.md` section 2. Y9-T and RS-0's grant wait close
both.] [Round 6: they do so only with two further rules: a report carries the reporter's incarnation and counts only
under the incarnation `p` positioned (Y9-T, `docs/AFR_001_REPORT_IDENTITY.md`), since a report sent before an in-place
reseed otherwise counts once `k` is grown back and grants again; and RS-0's wait precedes RS-1 and runs from the later
of the last grant and the boot, since the grant time does not survive a crash (contract 6.5).]

## 2. The invariants in U

**Lemma C (the copy is one primary's log), under premise P.** Every entry `(s, x)` in `k`'s log in U was in `j`'s shipped log at the
moment `k` received it, and `k`'s log is one contiguous slice of `j`'s log, from `start_lsn` to `k`'s end, as `j` held
it during one incarnation as primary of `t`.
- RS-3 to RS-5 copy contiguously from `start_lsn` (note C.2). Below `start_lsn`, `k` holds state, not entries, like
  any member after compaction (its `H_k` starts at the floor `(start_term, start_lsn − 1)`).
- During one incarnation as primary, `j` never truncates its own log (SA, I4 step 2), and it loses a shipped tail only
  by restarting (a new incarnation) or by a write error, after which it stops being primary (R6.0). Either ends the
  transfer (U2, U3). So the slices `k` received in turn all belong to one growing log.
- **Premise P (round 4):** the parts are `j`'s active checkpoint, which may have been taken in an earlier incarnation
  or term of `j`, at a fuzzy `max_lsn` that need not have been durable on `j`. The state they hold is `j`'s current
  log applied through `max_lsn` only if `j`'s own recovery refused whenever its WAL ended below that checkpoint's
  `max_lsn`, which is D-02's to close (boundary rule 7, C5). Counterexample without it: `j` checkpoints through `y`
  holding `(s, y)` only in memory, crashes, and later holds `(t′, y)`; `k`'s state then reflects `(s, y)` while its
  WAL holds `(t′, y)`. **Lemma C, and everything below that uses it, depends on P.** [The evidence that discharges P is listed in
  `docs/AFR_001_DURABILITY_DECISIONS.md` section 8 (P-1 to P-4).]

**I1 (uniqueness)** is untouched: it follows from R6.0 for every node.

**I2 (log matching).** SA argues I2 from the fact that a member's term-`s` entries are a contiguous run of the
term-`s` primary's log, starting where that primary started term `s`, and says the run starts there "because a member
accepts entries from a primary only after R6.2 has positioned it." For `k` in U the same fact holds by another route:
by Lemma C, `k`'s term-`s` entries are exactly `j`'s term-`s` entries within the slice, and `j`'s satisfy the fact
(`j` is a member that is not incomplete, and I2 holds for it by the induction). The run starts at `max(start of s,
start_lsn)`, the same truncation at the floor that every compacted member has, and which I2 already tolerates. So I2
holds for every pair involving `k` in U. **What changes in SA's text:** the sentence "because a member accepts entries
… only after R6.2 has positioned it" gains "or copies them as one slice of a primary's log under R6.5 (U2, U3)".

**I3 (truncation keeps every common entry).** The R6.2 round that ends U uses only I2's contiguity for `k` and the
answering primary `j′` (which may be `j` or a later primary). Both hold, so I3's argument applies unchanged: each
truncation point is at or above the longest common prefix, and the round ends with `k` a prefix of `j′`. If the cut is
below `c + 1`'s `max_lsn`, R6.4 applies: `k` is reseeded again (RS-1 compares on NONE and the term).

**I4 (acknowledged writes kept).** Step by step against SA's induction:
- **Acknowledgements (steps 1, 3).** Under U1 `k` sends no report, so no acknowledgement in any term counts `k`
  (R9-A′, R9-C). If `p` puts `k` in A under R1.1 before it positions, every acknowledgement waits for `k`: liveness
  only, and UNRESOLVED as a bound (R2′ rule 5 binds survivors in S′, not `k`; R1.2 permits a drop but never requires
  one). SA's I4 step 6 says an incomplete member is "outside A"; R9-C can keep it in A while a lease may be valid.
  Its conclusion needs only "never counted", which U1 gives.
- **Membership (steps 1, 3, 4).** `k` is not in S. It re-enters S only through R1's Grow, which needs "caught up",
  which needs R6.2 (SA section 2). So "every member of S holds `e`" never refers to `k` in U.
- **Promotion (steps 2, 4).** Automatic promotion and operator promotion inside S name a member of S (R2, R2′), so
  not `k`. R7 outside S could name `k`; a restarted standby outside S is promotable the same way today, and R7
  outside S is already I4's listed exception, with loss accepted (G-35). P6′'s "not reseeding" attestation does not
  block `k` in U. This is a new window compared with R6.5's "cannot be promoted", disclosed, not closed.
- **Interrupted truncation (step 5).** None is pending in U (RS-1 requires every stream NONE; RS-6 writes NONE).
- **Interrupted reseed (step 6).** SA: "An incomplete member is outside A and S and cannot be promoted, so it is
  never the only holder of `e` that counts." In U: outside S, never counted (U1), promotable only by R7 outside S.
  The conclusion holds except for I4's existing exception.

**Leases.** R9-B needs positioning, U4 ends any primary's record that `k` is positioned, U1 forbids holding or requesting one, RS-0 dropped earlier ones, and R9-C keeps
`k` in `p`'s A until any lease `p` granted it has expired. R9-F's `c_m` is reset while `k` is not positioned. So `k`
serves no lease read in U.

**I5 (crash atomicity).** RS-6 is one record write (four slots): a crash leaves either RESEEDING (recovery restarts
the reseed at RS-2) or NONE with `c + 1` published. A crash in U restarts `k` as a NONE node still in U: positioning is
volatile, its incarnation is new, and contract 2.2 step 12 positions it by R6.2 before it holds a lease. Its reports
before that rest on U1 carried across the restart; if they were counted anyway, the S argument above still holds. I5's list gains no new state: a node in U after a crash is
"its log" with nothing pending, the first of I5's states. A crash during the R6.2 round's truncation is I5's ordinary
case.

**Reconciliation and term changes.**
- A newer term learned before RS-6: `k` persists it (R4); RS-6's compare on `t` fails; back to RS-2.
- A newer term learned in U: `k` persists it (R4) and runs R6.2 against the new primary `j′`. Entries `k` copied
  from `j` that `j′` lacks were never counted (U1).
- `j` lost a tail it shipped to `k` (its restart, R6.0): `j` is no longer primary of `t`, and the next primary does
  not hold that tail. `c + 1`'s `max_lsn` is `k`'s end (U3), so any cut of the copied tail is below it, and R6.4
  reseeds `k` again rather than truncating. Safety is the same; every failover during a reseed costs a new transfer.
  This is why RS-6 is not a positioning (contract 6.6).
- Repeated reseeds: each needs a cut below `c + 1`'s `max_lsn`. No bound under repeated term changes (UNRESOLVED,
  liveness only).

## 3. A counterexample outside U (found while checking U1)

**Finding Y9 [INF, not model-checked]** (round 4 called it defence in depth; round 5 found delayed-report cases that
break I4 and G-11, `docs/AFR_001_REPORT_IDENTITY.md`). R9-A′, like R9-A, speaks
of "an entry", but reports carry an **LSN** (`shard.cpp:2234` sends `durable_lsn()` with no term), and nothing in the
frozen text says a report may be sent only while the replica is positioned. R2′ rule 2 says so for survivors in S′; R1.1 puts a new member `c` in A **before** it has caught up.
Scenario: `c` restarts holding `(s, x)`, an unacknowledged tail entry of an older term; `p`, primary of `t > s`, puts
`c` in A (R1.1) and creates `(t, x)`; `c` reports "applied and covered through `x`" before R6.2 has truncated its
`(s, x)`; `p` counts it. Section 0's "every member of A has `e`" is then false for `c`; G-11 is not broken, because G-12
keeps another positioned member of S that holds `(t, x)` (section 2). In `Log.tla`, `Ack` requires `pos` only with
`R2p ∧ FixPosAck`, compares entries by identity (`dlog[m][i] = dlog[p][i]`), and `AddA` requires `pos[c] = T[p]`,
stricter than R1.1's text; so the models never explored it.

[Superseded 2026-11-19 by `docs/AFR_001_REPORT_IDENTITY.md`: positioning alone fails under a delayed report; the
recommendation there is Y9-T, a report that names its entry `(term, lsn)`.] **First proposal (needs the owner's yes, Y9):** add to R9-A′ "… and only while it is positioned on p's log in
p's term (R6.2); p counts no report from a replica it has not positioned." This writes the model's guard into the text, and
gives U1's report clause a general rule. With U4, `p`'s side holds across an in-place reseed too. Not applied: R9-A′ is a recorded revision, and changing it
needs the owner.

## 4. Result

Under premise P and U1-U4, I1-I5 hold for a node in U, with one disclosed change: R7 outside S may promote `k` in U, where R6.5's
current text forbids promoting an incomplete node; that loss is already accepted under G-35. **Recommendation:** accept
Y6c-2 with U3 and U4 added to its text, once premise P is part of D-02's acceptance; Y9 as revised in `docs/AFR_001_REPORT_IDENTITY.md` (Y9-T). All rest
on argument only.

**What a yes changes:** R6.5's first sentence (contract 6.6, with U3 and U4; RS-1's new incarnation, contract 6.5); SA I2's "accepts entries" sentence; I4 step 6
(add U); R6.5's "cannot be promoted" applies to the flag only; R9-A′'s report condition (Y9).

**UNRESOLVED:** premise P (D-02); no model covers U, R1.1's add before positioning, or R7 outside S; degraded mode with `m = 1` (Y9's relevance) (`Log.tla`'s `RDone` positions in the same step); no bound on reseed loops; whether
reports should carry `(term, lsn)` instead of Y9's positioning guard.
