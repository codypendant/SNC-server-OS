# AFR-001: report/reseed model, preregistered plan (PROPOSED, NOT AUTHORIZED TO RUN)

Status: **PROPOSED plan, for Codex and owner review.** Docs only. **No model file is written and nothing is run.**
Writing the model, any sizing run and every counted run need the owner's explicit yes. Asked by Codex's review of
#175 (`docs/handoff/PR175_Codex_Review.md`, section 3). Nothing here accepts Y9, Y6c-2 or E32. Revised once after an
independent review told to break it (section 8).

**Why a model.** Rounds 5-7 found a counterexample in each previous round's version of the report, reseed and
grant rules (`docs/AFR_001_DURABILITY_DECISIONS.md` 11.2). Those are interleavings of delayed messages, crashes and
term changes, which prose review finds one at a time. The model asks one question: **with the rules proposed today,
can a primary acknowledge an entry, or grow a member, on the strength of a report that is not true, and does each
rule matter?**

## 1. What is modelled

**Base.** `spec/tla/ReconBlockers.tla`, unchanged where possible: the witness record, terms, promotion (R2′, R7
inside S), R6.2 reconciliation with replies as a set (delayed, duplicated, reordered, lost, surviving restarts) and
its binding, pending intents (R6.3′ item 3), crashes, standby leases (untimed), Grow, Shrink, Drop. Its proven rules
stay on. New module: `spec/tla/ReportReseed.tla`. `MaxIds` is the base's bound on entries ever created (called
"LSNs" by Codex). Changes:

1. **Reports are messages.** A replica `m` sends a report to any node; it joins a set `msgs` and stays there, so it
   can be delivered to any node, any number of times, in any order, or dropped, before or after any crash. A report
   carries `[e_t, e_l, inc, term]`: the term and LSN of the last entry `m` holds durably, `m`'s incarnation and
   `m`'s durable term at send. At most `MaxMsgs` reports are outstanding. Reports may come from unpositioned
   replicas (`docs/AFR_001_REPORT_IDENTITY.md` section 2).
2. **Counting.** A primary `p` records `cov[p][m] = e_l` on delivery only if `m` has a positioning record at `p`
   (`pinc[p][m] ≠ NONE`) and:
   - `FixEntry`: `p`'s log holds an entry of term `e_t` at `e_l`;
   - `FixInc`: `inc = pinc[p][m]`;
   - `FixTerm`: `term = T[p]`.
3. **The positioning record.** `pinc[p][m]` is set when `p` answers `m`'s R6.2 request "positioned", to the
   incarnation in that request. It ends (`NONE`) when a request from `m` arrives under another incarnation (RS-3, a
   new boot), and when `p` crashes, learns a newer term or is promoted. **`cov[p][m]` is reset to 0 whenever
   `pinc[p][m]` is set or ends** (W-2, section 6: the text does not yet say this).
4. **Decisions by `p` read only `pinc` and `cov`,** never `m`'s state: AddA, Grow (`cov[p][k] ≥ max(inh_p, pack_p)`,
   the pessimistic reading of "caught up", UNRESOLVED in the text), SGrant (R9-B, the same test) and Ack.
5. **Acknowledgement** (R9-A′, R9-C): `p` acknowledges entry `i` if `p` is primary (not necessarily current, so a
   deposed primary that still holds grants is modelled), `|A[p]| ≥ 2` (a permissive form of `p`'s view that
   `|S| ≥ m = 2`), every member `m ≠ p` of `A[p]` has `cov[p][m] ≥ i` and `p ∈ gr[m]`, and `p` holds `i`.
6. **Grants (authority, R3, R4) untimed.** `gr[k]` is the set of primaries holding a valid grant from `k`.
   `Grant(k, p)` requires `T[k] = T[p]`, `k` not RESEEDING, and a boot read (R7) since `k`'s last crash.
   `Expire(k, p)` removes `p` at any time. `p` drops `k` from `A` only after `p ∉ gr[k]` (R9-C). A grant survives
   `k`'s crash, because it lives at `p`.
7. **Reseed in place** (contract 6.5 as amended; the R6.5 revision):
   - `StartReseed(k)` for any `k ∉ rec.S`, standing for an operator reseed or one after a refusal; the base's R6.4
     trigger stays.
   - RS-0 stops granting. With `FixWait`, RS-1 (durable RESEEDING and a new incarnation) needs `gr[k] = {}`, the
     untimed form of "`W` after the later of the last grant and the lock time".
   - RS-2 discards.
   - RS-3 persists `j`'s term and sends a request under the new incarnation (which ends `j`'s record, item 3).
   - RS-5 installs a **prefix of `j`'s durable log whose length TLC chooses**, standing for "through `max_lsn`"
     with entries still in flight.
   - RS-6 clears the flag without positioning; an ordinary R6.2 round follows.
   - A crash may happen between any two steps; recovery with RESEEDING resumes at RS-2.
8. **Incarnation.** `incn[n] = <<nonce, gen>>`, with `gen = 1` at Init and after a wipe. `gen` grows at every boot
   write and at RS-1. `Wipe(k)`, for `k ∉ rec.S` with `gr[k] = {}` (premise W-1), models an operator wipe and fresh
   seed: the log becomes a source prefix of TLC's choice, `gen = 1`, and with `FixNonce` a fresh nonce; without it,
   the old nonce is kept.
9. **R7 outside S.** `OpOutside(j)`: with `rec.ready` and `|rec.S| = 1`, the operator promotes a `j ∉ rec.S` (R7
   case 1). `r7T` is the highest term R7 outside S has created. On in both lanes.
10. **Sender rule.** `FixSendP`: no report while an intent is pending. `FixSendR`: no report while RESEEDING.

**Mutations** turn one rule off, every other rule on: M1 `FixInc`, M2 `FixTerm`, M3 `FixWait`, M4 `FixEntry`,
M5p `FixSendP`, M5r `FixSendR`, M6 `FixNonce`.

## 2. Invariants and probes

Both safety properties are checked **at the moment of the decision**, through a history variable written by the
Ack and Grow steps, not as properties of every later state. G-11 counts copies "at the time of the acknowledgement"
(`docs/SNC_GUARANTEES.md` 136), and G-12 allows later degraded states with one copy (137).
- **`AckSound`** (G-11, `m = 2`): when `p` acknowledges `i`, every member of `A[p]` durably holds `p`'s entry `i`
  and is not RESEEDING, and at least two nodes hold it.
- **`GrowSound`** (I4 at admission): when `p` grows `k` into S, `k` durably holds `p`'s log through
  `max(inh_p, pack_p)`.
- **`AckedPreserved`** (I4, from the base): every acknowledged entry, except those acknowledged in a term below
  `r7T` (SA's stated exception for R7 outside S, applied only to earlier acknowledgements), is held by every member
  of `rec.S`.
- From the base, unchanged: `EntryUnique` (I1), `LogMatching` (I2), `HistoryUnique` (G-15).
- `Safety` is the conjunction.

**Probes.** These are reachability checks. TLC must report each one violated, or the lane does not exercise what
it claims.
- One probe per check, each a report rejected **only** by that check, the other two passing: `NoRejectByInc`,
  `NoRejectByTerm`, `NoRejectByEntry`.
- `NoShortReseed`: RS-6 completes with a log shorter than a report from the same node still in `msgs`.
- `NoWipe` (lane A), `NoAckAfterR7`: an acknowledgement in a term at or above `r7T > 0` (lane B, else
  `AckedPreserved`'s exception makes it vacuous), `NoCrashInReseed` (lane A).

## 3. Lanes, bounds and expected results

Setup as `spec/tla/README.md`: `tla2tools.jar` v1.8.0 (sha256 there), Java 21, `-deadlock -Xmx4g`, one worker, at
most three runs at a time. `states/` is deleted after every run, and the disk is checked before each launch.

- **Lane A:** `Nodes = {n1, n2, n3}`, `MaxTerm 2`, `MaxIds 2`, `MaxCrashes 1`, `MaxMsgs 2`, `MaxReseeds 1`,
  `MaxWipes 1`.
- **Lane B:** `Nodes = {n1, n2, n3, n4}`, `SYMMETRY` over the nodes other than `P0`, `MaxTerm 3`, `MaxIds 2`,
  `MaxCrashes 1`, `MaxMsgs 1`, `MaxReseeds 0`, `MaxWipes 0`.

Each expected counterexample below is the shape the review walked by hand [INF]. None needs a crash, except where
stated.

| Run | Lane | Change | Expected |
|---|---|---|---|
| CA | A | none | `Safety` holds |
| CB | B | none | `Safety` holds |
| M1 | A | `FixInc` off | violated: B-6a. A report sent before an in-place reseed counts after the partial copy and a new positioning; `AckSound` or `GrowSound` |
| M2 | B, also seen in A | `FixTerm` off | violated: B-7a. `k` is truncated against `j′` in term 2, and its term-1 report is counted by a node promoted outside S in term 3 (in lane A that node can be the original primary) |
| M3 | A | `FixWait` off | violated: B-6b without the crash. RS-1 and RS-2 run while `p` holds `k`'s grant, and a stale report counts at the ack (`AckSound`). **The round-6 defect, the wait lost in a crash, is not visible to an untimed guard and is not checked** |
| M4 | A | `FixEntry` off | violated. `k` reports `(1, 2)` unpositioned in term 2, is truncated by R6.2 to 1, and is positioned. The term-2 primary then writes `(2, 2)` and counts the report (`AckSound`). Round 7's draft predicted "not necessary"; the review showed that wrong |
| M5p | B | `FixSendP` off | violated: an intent is still pending when `k` reports in term 3. The intent then truncates `k`, and the new primary positions `k` under the same incarnation and counts the report |
| M5r | A | `FixSendR` off | violated: between RS-1 and RS-2 `k` reports under its new incarnation. The partial copy and R6.2 follow, and the report counts |
| M6 | A | `FixNonce` off | violated: B-7b. A wipe and fresh seed reuse `(nonce, 1)`, and a pre-wipe report counts after the new positioning |
| P1-P6 | A | none | `NoRejectByInc`, `NoRejectByTerm`, `NoRejectByEntry`, `NoShortReseed`, `NoWipe`, `NoCrashInReseed` violated (reachable) |
| P7, P8 | B | none | `NoRejectByTerm`, `NoAckAfterR7` violated (reachable) |

That is 17 launches.

**Sizing** is a 6-minute run of each lane at its counted bounds, not counted and listed with the results. The base
model's own lanes needed 29 minutes to 89 minutes at smaller scopes (`docs/AFR_001_RECON_BLOCKERS.md`, C2 and C3), so
lanes A and B may not finish within the cap. **The fallbacks are fixed now, before any run.** The sizing result
only decides which fallback a run uses:
- CAf: lane A with `MaxMsgs 1`.
- CBf: lane B with `MaxCrashes 0`, keeping R7, so M2 stays reachable.
- Mf: a mutation's lane with `MaxMsgs 1`.

A fallback result is reported as the smaller check it is.

## 4. Abstractions (limits stated before any run)

- **Time:** grants and leases are untimed. The RS-0 wait is the guard `gr[k] = {}`. That `W = d·(1+ρ)` achieves it
  is R3's argument, checked in `Authority.tla`. That the wait survives a crash is argued in contract 6.5 and not
  checked.
- **Durability:** the durable log stands for applied and watermarked state (R9-A′). There are no media faults (D-02
  on HOLD), no torn tails and no buffered-WAL case.
- **Reseed:** the copy is a prefix of the source's log (premise P, B9, assumed). Checkpoints are folded into the
  log.
- **Other simplifications:** Witness Paxos is one atomic record. There is one stream. The R6.2 binding is the base's
  log-snapshot abstraction (`docs/AFR_001_RECON_BLOCKERS.md` 2.1). U1 (no reports, leases or S while in state U) is
  covered only through the counting rule, not as a separate guard.
- **A PASS** covers only these bounds and this scope. It is evidence for the owner decision on Y9, not proof.

## 5. Stop rules and run budget

As `docs/AFR_001_RECON_BLOCKERS.md` 2.5, with these:
- **Correct-protocol violations:** any violation in CA or CB blocks Y9-T as written, whether or not it is explained.
- **Mutation counterexamples:** read state by state. A shape other than the expected one is reported and explained.
  An unexplained one is a blocker.
- **Mutations that do not fail:** a completed mutation run that finds nothing is reported as **not shown
  necessary**. A run that hits the cap is **NOT COMPLETED**, never "not shown necessary" and never a pass.
- **Budget:** 17 counted launches, at most 3 fallback launches and at most 6 reruns. The cap is **2 h** of wall
  clock per run.
- **What may change after a result:** bounds are never raised. Invariants and expectations are not changed except
  through a recorded model-defect or protocol finding, and each such rerun is counted.
- **When the budget runs out:** the rest is NOT RUN, and the work stops for Codex and the owner.
- **Before counted runs:**
  1. Write the model and its `.cfg` files.
  2. Have one independent reviewer, told to break the model's fidelity to section 1, review it.
  3. Run the sizing runs.
  4. Commit this file's final text and the model before the first counted launch.

## 6. Text gaps found while planning (protocol items, not model items)

- **W-1, operator wipe window:** if an operator wipes a node within `d` of its process stopping, `p` can count a
  pre-wipe report under a grant that is still valid, whatever the nonce. The model assumes `gr[k] = {}` at a wipe.
  The text has no rule; the candidate is a fresh seed that waits `W` after taking the lock before its first R6.2
  request, plus an operator note. Proposed only.
- **W-2, coverage belongs to its positioning record:** Y9-T checks identity only at the moment of counting. A
  coverage counted before a reseed would otherwise survive the end of the positioning record and count again once
  `k` grants after RS-6, with no delayed message involved. Proposed text: "`p`'s counted coverage of `m` belongs to
  its positioning record of `m` and ends with it; it also ends when `p` learns a newer term." The model encodes it
  (section 1 item 3).
- Whether acknowledgements in terms after R7 outside S fall under I4. The plan takes the strict reading for
  `AckedPreserved`; `AckSound` is unaffected.
- **Not checked:** liveness, and the storage-side round-7 items (step 9a, `E`, explicit `start_lsn`, new-stream
  initialization). The storage items are code facts and stay design requirements for the 10.1 code changes and
  crash tests.

## 7. The owner's decision

**Asked: yes or no to running this plan as written.** A yes covers:
1. writing the model;
2. one fidelity review;
3. the uncounted sizing runs;
4. the 17 counted launches, with fallbacks and reruns within the budget.

It does not accept Y9, Y6c-2, E32, W-1 or W-2, and changes no wire format or code.

## 8. Review of the first draft (one reviewer, told to break the plan)

6 BLOCKER and 7 MAJOR findings, all taken up above. The fixes are not re-reviewed.

| # | Finding | Now |
|---|---|---|
| B1 | `TwoCopies` as a property of every state fails on the correct protocol (legal degraded mode after a later shrink and reseed) | `AckSound` at the moment of the ack |
| B2 | `CountTrue` fails on the correct protocol: a grant can outlive a term change while the replica is truncated, though `p` cannot then acknowledge | removed; `AckSound` and `GrowSound` |
| B3 | an atomic, exact copy makes M1 and M6 unreachable (the stale report becomes true again) | RS-5 and the seed install a prefix of TLC's choice |
| B4 | nothing reseeded a node that had not diverged | `StartReseed(k)` for `k ∉ rec.S`; `Wipe` needs the same guard |
| B5 | M4's "not necessary" prediction was wrong: a report sent while unpositioned passes the term and incarnation checks | M4 expected to fail |
| B6 | M5's prediction was wrong (completing an intent changes neither term nor incarnation), and its RESEEDING half was vacuous in lane B | split into M5p (lane B) and M5r (lane A), both expected to fail |
| M1 | counted coverage outlived its positioning (no reset) | reset with the record; text gap W-2 |
| M2 | grants had no term rule | `T[k] = T[p]` and a boot read |
| M3 | B-7a's delivery to another node was unreachable with addressed reports | delivery to any node |
| M4 | `NoStaleCount` was vacuous (a disjunction) | one probe per check, plus `NoShortReseed` |
| M5 | the round-6 B-6b defect (wait lost in a crash) is invisible to an untimed guard | stated as not checked |
| M6 | the base's oracle guards (Follows) would hide stale counts; R9-B was judged from `m`'s log | `p`'s decisions read only `pinc` and `cov` |
| M7 | the base needed up to 89 minutes at smaller scopes; capped runs could read as "not necessary"; sizing at `MaxLsn 1` sizes nothing | lane A at `MaxTerm 2`, lane B at `MaxCrashes 1`; capped = NOT COMPLETED; sizing at counted bounds; fallbacks fixed now |

**MINOR fixes:** `MaxIds` defined, `r7T` the highest R7 term, OpOutside on in both lanes, U1 stated as not a
separate guard, `gen = 1` at Init and after a wipe, `|A| ≥ 2` stated as permissive.
