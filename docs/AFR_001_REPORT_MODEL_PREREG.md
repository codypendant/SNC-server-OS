# AFR-001: report/reseed model, preregistered plan

**Result (section 12): STOPPED before any counted run.** The three-way fidelity check broke the correct protocol
with every rule on: two TLC-confirmed `AckSound` counterexamples, F-1 found while writing, and one hole the model
cannot express. By the stop rule (section 5) this **blocks Y9-T as written**. No counted launch was made, so none of
the 18 is spent. Section 12.4 recommends a redesign, which needs the owner's yes.

Status of the plan text below: **AUTHORIZED and FINAL before the counted runs.** The owner gave the explicit GO on 2026-11-19 16:06 UTC;
Codex's #177 review (`docs/handoff/PR177_Codex_Review.md`) approved the plan. Section 11 records what writing the
model found before any counted run: one protocol finding (F-1), a proposed rule W-3 with one more mutation (M7, 18
launches), and the modelling choices. Nothing here accepts Y9, Y6c-2, E32, W-1, W-2 or W-3. The model is
`spec/tla/ReportReseed.tla`; `spec/tla/report_reseed_cfg.sh RUN` prints each run's config.

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

1. **Reports are messages.** A replica `m` sends a report; it joins the set `msgs` and stays there, so it can be
   delivered to any node, any number of times, in any order, or dropped, before or after any crash. A report is an
   immutable record `[from, e_t, e_l, inc, term]` whose `from` names its sender (Codex #176 B); nothing ever edits a
   report in `msgs`. It carries: the term and LSN of the last entry `m` holds durably, `m`'s incarnation and
   `m`'s durable term at send. At most `MaxMsgs` reports are outstanding. Reports may come from unpositioned
   replicas (`docs/AFR_001_REPORT_IDENTITY.md` section 2).
2. **Counting.** A primary `p` delivered a report `r` with `r.from = m` records `cov[p][m] = r.e_l` only if `m` has
   a positioning record at `p` (`pinc[p][m] ≠ NONE`) and:
   - `FixEntry`: `p`'s log holds an entry of term `e_t` at `e_l`;
   - `FixInc`: `inc = pinc[p][m]`;
   - `FixTerm`: `term = T[p]`.
3. **The positioning record.** `pinc[p][m]` is set when `p` answers `m`'s R6.2 request "positioned", to the
   incarnation in that request. It ends (`NONE`) when a request from `m` arrives under another incarnation (RS-3, a
   new boot), and when `p` crashes, learns a newer term or is promoted. **`cov[p][m]` is reset to 0 whenever
   `pinc[p][m]` is set or ends** (W-2, section 6: the text does not yet say this).
4. **Decisions by `p` read only `pinc` and `cov`,** never `m`'s state: AddA, Grow (`cov[p][k] ≥ max(inh_p, pack_p)`, with the Grow's chosen S joining `sv[p]` only when `p` learns it,
   the pessimistic reading of "caught up", UNRESOLVED in the text), SGrant (R9-B, the same test) and Ack.
5. **Acknowledgement at HA_DURABLE, faithful to the text** (SA "Ack set A and lease", R1, G-12, R9-A′; Codex #176
   A). `p` acknowledges entry `i` only if all of:
   - `p` is primary (not necessarily current: a deposed primary that still holds grants is modelled);
   - **normal-mode authority:** `p ∈ gr[m]` for every `m ∈ A[p] \ {p}`;
   - **`p`'s own view of S**, `sv[p]`, has `|sv[p]| ≥ m = 2` and `sv[p] ⊆ A[p]`;
   - every `m ∈ A[p] \ {p}` has `pinc[p][m] ≠ NONE` and `cov[p][m] ≥ i`;
   - `p` holds `i` durably.

   `sv[p]` is set to the chosen S at `p`'s promotion and changes only when `p` learns a chosen version: the reply to
   its own Grow or Shrink, or a read of R at its own term (R1.2). A lost reply leaves `sv[p]` unchanged, which is how
   a primary "misses a shrink". `Drop(p, c)` needs `c ∉ sv[p]` and R9-C. Degraded-mode acknowledgements (`|S| = 1`,
   witness authority, R8) are not HA_DURABLE and are not modelled. **There is no permissive lane**: every lane is
   the faithful one, so every counterexample is one the text allows.
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
M5p `FixSendP`, M5r `FixSendR`, M6 `FixNonce`, and M7 `FixGrow` (W-3, section 11).

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
| M7 | A | `FixGrow` off (W-3 (a), section 11) | violated: F-1. A report counted before RS-1 lets `p` grow `k` into S after RS-2 discarded the entry (`GrowSound`) |
| P1-P6 | A | none | `NoRejectByInc`, `NoRejectByTerm`, `NoRejectByEntry`, `NoShortReseed`, `NoWipe`, `NoCrashInReseed` violated (reachable) |
| P7, P8 | B | none | `NoRejectByTerm`, `NoAckAfterR7` violated (reachable) |

That is 18 launches (17 planned, plus M7 from section 11).

**Sizing** is a 6-minute run of each lane at its counted bounds, not counted and listed with the results. The base
model's own lanes needed 29 minutes to 89 minutes at smaller scopes (`docs/AFR_001_RECON_BLOCKERS.md`, C2 and C3), so
lanes A and B may not finish within the cap. **The fallbacks are fixed now, before any run.** The sizing result
only decides which fallback a run uses:
- CAf: lane A with `MaxMsgs 1`.
- CBf: lane B with `MaxCrashes 0`, keeping R7, so M2 stays reachable.
- Mf: a mutation's lane with `MaxMsgs 1`.

A fallback result is reported as the smaller check it is.

**Message bounds and delivery (Codex #176 B).** Sending is enabled at any time the sender rules allow, delivery
never consumes a report, and `DropMsg` removes any report, so a full `msgs` never blocks a needed send: TLC can drop
an unneeded report first. The bound only limits how many are in flight at once. Each expected counterexample, walked
by hand [INF], needs at most this many stale reports in flight at the moment they count, against the lane's bound:

| Run | Reports in flight needed | Lane bound |
|---|---|---|
| M1 (B-6a) | 1 stale (before the reseed) + 0 fresh (the Grow and ack use it) | 2 |
| M2 (B-7a) | 1 stale (term 1) | 1 (lane B), 2 (lane A) |
| M3 (B-6b) | 1 stale | 2 |
| M4 | 1 stale (sent while unpositioned) | 2 |
| M5p | 1 stale (sent with an intent pending) | 1 |
| M5r | 1 (sent between RS-1 and RS-2) | 2 |
| M6 (B-7b) | 1 stale (before the wipe) | 2 |
| M7 (F-1) | 1 stale (before RS-1) | 2 |

Fresh reports needed for the ack itself come from the same sender and replace nothing, since the stale report is
the one that counts. If the fidelity review finds a trace needing more, the plan is corrected **before** any counted
run, never after a result.

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

### 4.1 Assumption register: everything a PASS depends on (Codex #176 B)

| # | Assumption | Status in the text | If false |
|---|---|---|---|
| AS-1 | **W-2:** counted coverage ends with its positioning record and when `p` learns a newer term | **proposed, not accepted** | a PASS says nothing about Y9-T without W-2 |
| AS-2 | **W-1:** no wipe while a grant from the node may be valid | **proposed, not accepted** (no rule yet) | M6 and any wipe trace are unchecked against an early wipe |
| AS-3 | the RS-0 wait achieves "no grant from `k` valid at any holder" before RS-1 | argued (R3; contract 6.5); untimed here | **the real-time wait, including across crashes, is not checked** |
| AS-4 | premise P: a reseed copies a prefix of one primary's log | B9, open | Lemma C and the copy model fail |
| AS-5 | no media faults, torn tails or buffered-WAL failures | D-02 on HOLD | out of scope |
| AS-6 | witness Paxos is one atomic record; R6.2's binding is the base's log snapshot | as the base (`docs/AFR_001_RECON_BLOCKERS.md` 2.1) | as the base |
| AS-7 | grants need `T[k] = T[p]` and a boot read since the last crash | frozen R3, R4, R7 | a model defect, not a protocol finding |
| AS-8 | "caught up" (Grow, R9-B) judged from counted reports | UNRESOLVED in the text; pessimistic reading | a PASS holds under either reading |
| AS-9 | only HA_DURABLE acknowledgements; `sv[p]` as in section 1 item 5 | SA, R1, G-12 | degraded acknowledgements unchecked |
| AS-10 | one stream; at most `MaxIds` entries ever created; the stated bounds | scope | a PASS covers these bounds only |

## 5. Stop rules and run budget

As `docs/AFR_001_RECON_BLOCKERS.md` 2.5, with these:
- **Correct-protocol violations:** any violation in CA or CB blocks Y9-T as written, whether or not it is explained.
- **Mutation counterexamples:** read state by state. A shape other than the expected one is reported and explained.
  An unexplained one is a blocker.
- **Mutations that do not fail:** a completed mutation run that finds nothing is reported as **not shown
  necessary**. A run that hits the cap is **NOT COMPLETED**, never "not shown necessary" and never a pass.
- **Budget:** 18 counted launches (section 11), at most 3 fallback launches and at most 6 reruns. The cap is **2 h** of wall
  clock per run.
- **What may change after a result:** bounds are never raised. Invariants and expectations are not changed except
  through a recorded model-defect or protocol finding, and each such rerun is counted.
- **When the budget runs out:** the rest is NOT RUN, and the work stops for Codex and the owner.
- **Before counted runs (Codex #176 C):**
  1. Write the model and its `.cfg` files.
  2. **Three-way fidelity check of the `.tla` text itself**, not a summary: the author and **two** independent
     reviewers, each told to break it, check every action and invariant against sections 1-2 and the register.
     Each finding is fixed and recorded, and a disagreement is recorded as UNRESOLVED.
  3. Then the sizing runs.
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
2. the three-way fidelity check;
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

## 9. Codex's #176 corrections

| Item | Correction |
|---|---|
| A | Ack is now the text's rule: normal-mode authority from every member of A, and `p`'s own view of S with `|sv[p]| ≥ 2` and `sv[p] ⊆ A[p]`. A missed shrink is a lost reply that leaves `sv[p]` stale. Degraded acknowledgements are excluded. There is no permissive lane. |
| B | Reports are immutable records naming `from`; coverage is keyed by sender and reset with its positioning record (W-2, AS-1); message bounds are checked against every expected trace (table in section 3); every assumption a pass depends on is in 4.1, with W-1 and W-2 marked proposed. |
| C | The `.tla` text gets a three-way fidelity check (author plus two reviewers) before sizing and before any counted run, and is committed with this file first. |

The untimed RS-0 guard's limit is AS-3: it does not check the real-time wait or the wait across crashes.

## 10. What is not changed

The scope, bounds, lanes, the 17 launches, mutations, probes, invariants (`AckSound`, `GrowSound`,
`AckedPreserved`, `EntryUnique`, `LogMatching`, `HistoryUnique`), caps, fallbacks and stop rules are as in sections
1-5. Nothing is written or run until the owner's explicit yes.

## 11. Writing the model (before any counted run)

The model was written, then exercised with uncounted random simulation (`tlc2.TLC -simulate -depth 80`, 90-150 s
per config) to debug it. Simulation is not part of the campaign and proves nothing; it found one protocol problem
and two modelling defects, all before the fidelity review and before any counted launch.

**F-1 (protocol finding, against the text).** Simulation of CA as first written violated `GrowSound`, and with
that fixed, `AckedPreserved`:
- *Shape 1.* `k` reports entry `(1, 1)` under incarnation `(0, 1)` while positioned at `p`. `k` starts a reseed:
  RS-0, the grant wait, RS-1 (new incarnation `(0, 2)`), RS-2 discards the log. `p`'s positioning record of `k`
  still names `(0, 1)`, because it ends only when a request under the new incarnation reaches `p` (RS-3). `p`
  delivers the old report (it matches the record), counts coverage 1, and grows `k` into S with an empty log.
- *Shape 2.* `k` passes RS-0's check `k ∉ S`, and `p` grows `k` into S under a grant `k` gave before RS-0. The wait
  then ends, RS-1 and RS-2 run, and a member of S has discarded acknowledged entries.

The grant wait protects acknowledgements, because an ack needs a valid grant from every member of A. **S admission
(Grow, "caught up") needs no grant in the text,** so neither Y9-T nor RS-0's wait covers it. This contradicts the
round-5 note in `docs/AFR_001_R65_INVARIANTS.md` section 0, which says Y9-T and the grant wait close the
stale-report Grow case. It holds under AS-8's pessimistic reading ("caught up" judged from counted reports); under
the other reading (judged at the end of the R6.2 request) the stale positioning record has the same effect.

**W-3 (proposed rule, not accepted).** (a) `p` grows `k` into S only while it holds a valid authority grant from
`k`, as for an acknowledgement. (b) RS-1 checks `k ∉ S` after the grant wait, not only at RS-0. With (a), no grant is
valid from RS-1 on (FixWait), and RESEEDING grants nothing, so no Grow of `k` can happen between RS-1 and RS-3.
With (b), a Grow before the wait ends is seen. The model's correct protocol now includes W-3. **M7** turns (a) off
(`FixGrow`); (b) stays on in every run. M7 adds one counted launch (18). Fallback and rerun budgets are unchanged.

**Modelling choices fixed while writing (for the fidelity review):**

| # | Choice | Why |
|---|---|---|
| FD-1 | Grow adds `c` to `sv[p]` at once; a lost reply can still leave a Shrink unseen | the first draft let a lost Grow reply make `p` drop a member of S from A and ack without it. A primary must treat its own Grow as possibly chosen; that is the text's Drop rule, `c ∉ S` |
| FD-2 | A wipe and fresh seed from `j` ends `j`'s record of `k` | the seed is a transfer from `j` under `k`'s new incarnation, as RS-3 |
| FD-3 | SGrant uses only `CaughtUp` (section 1 item 4), not the base's `Follows` | `p`'s decisions read only `pinc` and `cov` |
| FD-4 | Promotion skips a node holding the reseed lock; the lock survives a crash | the lock is durable (contract 6.5) |
| FD-5 | R6.2 ends `p`'s record only when it positions `k` or the request names another incarnation | section 1 item 3 |
| FD-6 | RS-5 and RS-6 need the source still primary of `k`'s term, else back to RS-2 | U2 |
| FD-7 | `MaxReps = 2` in both lanes | the base's value; section 3 did not name it |
| FD-8 | A rejected report changes only the probe record | the report stays in `msgs` |

**Uncounted simulation after these fixes** (random, not exhaustive, so absence means nothing): CA and CB found no
violation in 150 s. M1, M3, M4, M5r and M7 violated `Safety`; M2, M5p and M6 found none in 150 s. Probes P1-P4 and
P6-P8 were reached; P5 was reached earlier. M1's trace is B-6a.

## 12. Fidelity check result: the campaign stops before counted runs

The author's pass, then two independent reviewers told to break the `.tla` text (one on plan vs model, one on model
vs protocol text). Both reviewers replayed traces in TLC on fixed action sequences over the unmodified model; I
replayed both again (FACT, `spec/tla/ReportReseedTrR7.tla` and `ReportReseedTrW.tla`, each `.cfg` beside it, every
`Fix*` TRUE). A fixed sequence of the model's own actions is a behaviour of the full model, so each violation is a
counterexample to CA.

### 12.1 The correct protocol fails

| # | Shape | Evidence | Rule the text lacks |
|---|---|---|---|
| F-1 | `k` is grown into S during or just before a reseed, on coverage counted before RS-1 (section 11) | simulation while writing [FACT] | S admission needs no grant; nothing re-checks `k ∉ S` after RS-0 |
| F-2 | **R7 outside S.** `n1` (term 1) misses its shrink of `n2`, so its view of S stays `{n1, n2}`. The operator promotes `n3` outside S. `n2` grants to `n1` in term 1, reports entry 2, and `n1` counts it. `n2` learns term 2 and R6.2 against `n3` truncates entry 2. `n1` still holds `n2`'s grant and acknowledges entry 2, held by `n1` alone | `ReportReseedTrR7`, 21 steps, no crash, `AckSound` violated [FACT] | `n2` discards an entry while a grant it gave may be valid. R6.2 needs no grant, and W-2 resets coverage when `p` learns a term, not when the reporter does |
| F-3 | **Wipe seeded from a deposed primary.** `n2` (term 2) positions `n3` and counts its report of entry 1. `n3` is wiped and seeded empty from `n1`, still primary of term 1 (`gr[n3] = {}`, so W-1 holds). `n3` learns term 2, boot-reads and grants to `n2`. `n2`'s positioning record of `n3` was never ended, so `n2` acknowledges entry 1 with `n3` holding nothing | `ReportReseedTrW`, 16 steps, `AckSound` violated [FACT] | a wipe ends no primary's record except the seed source's; a grant says nothing about the incarnation that gives it |
| H-1 | **Delayed R6.2 request.** A request sent under incarnation `i` before RS-1 arrives after RS-3. `p` answers "positioned" and records `i` again; reports under `i` count, and after RS-6 `k` grants legitimately | argued [INF]: the model answers requests atomically, so it cannot express this | incarnations are compared only for equality (B-7b), so `p` cannot tell old from new |

Section 6's claim that `AckSound` is unaffected by R7 outside S is wrong (F-2). The round-5 note in
`docs/AFR_001_R65_INVARIANTS.md` section 0, that Y9-T and the grant wait close the stale-report Grow case, is wrong
(F-1).

### 12.2 W-3 does not survive the real protocol

W-3 holds in the model only because Grow and RS-1 are each one atomic step. In the real protocol a Grow
compare-and-set proposed while `p` still held `k`'s grant can be chosen after `k`'s wait, after `k` read `k ∉ S`, and
after RS-1. So W-3 (b) as a read is a check-then-act race [INF, reviewer B].

### 12.3 Model defects the reviews found (recorded, not fixed, since no counted run follows)

- Requests are not messages (H-1). RS-5 and Wipe copy the source's durable log, not its shipped log, and RS-6 checks
  that the source still serves, which `k` cannot see; the text's RS-6 compares only the term. So a deposed source's
  lost tail cannot happen.
- SGrant is a decision in scope but no property checks it (the base's `StandbyFresh` and `LeasedPrefix` were
  dropped), and it can grant a lease after RS-0.
- Mf did nothing in lane B, which already has `MaxMsgs 1`. The generator now uses `MaxCrashes 0` there and refuses
  unknown run names.
- Minor: Promote dropped the base's `rec.ready ∨ OpInside` (equal under `OpInside = TRUE`); Drop's `p ∉ gr[c]` is
  model-added, not R9-C; the R6.4 trigger is covered by RS-0 for any `k ∉ S`, so section 1 item 7's "stays" is wrong;
  `okE` reads `p`'s in-memory log.

Mutation reachability, as far as checked: M2, M5p and M6 were each reproduced by reviewer A in TLC on fixed
sequences; M1, M3, M4, M5r and M7 by uncounted simulation. With CA failing, a counted mutation run would mostly
re-find F-1 to F-3, so none was run.

### 12.4 Why the identity approach keeps leaking, and the recommendation

Every finding since round 5 has the same root. `p` decides from a **cached belief about `k`** (a positioning record
plus counted coverage), and `k` can invalidate that belief by an event `p` does not see: a reseed, a wipe, learning a
term and truncating, or a request arriving late. Each patch (incarnation, term, W-1, W-2, W-3) closes one event and
leaves the next.

**Recommendation [INF]: tie every count to a live promise, not to a cache.** Two rules:
- **P1. Grants name the granter's incarnation.** `p` counts `m` for an acknowledgement or a Grow only under a valid
  grant from `m` whose incarnation equals the one on the counted report and positioning record.
- **P2. Nothing is discarded under a live promise.** A node removes no durable entry (R6.2 truncation, RS-2, a wipe)
  while a grant it gave may still be valid. RS-0's wait and W-1 become two cases of this one rule.

P1 closes F-3 and H-1 (an old-incarnation record needs an old-incarnation grant, and none is valid after the wait).
P2 closes F-2 (`n2` cannot truncate inside its term-1 grant) and F-1's first shape. F-1's second shape and 12.2 need
one more rule: **P3.** RS-1 is preceded by a compare-and-set on R by `k` that bumps R's version, so a Grow proposed on
an older version cannot be chosen.

**Cost.** The grant message gains a field (a wire change). A node rejoining after a term change waits up to `W`
before truncating (rejoin latency, not acknowledgement latency). Grow gains a grant precondition (an R1.1 change),
and RS-1 gains a witness write. An uncounted 4-minute random simulation of P1 and P2 in a scratch copy found nothing,
which proves nothing: random simulation also missed F-2 and F-3.

**This is an architectural change** (wire format, R1.1, the reseed and truncation rules), so it needs the owner's yes
and Codex's review. The next step proposed is a re-preregistered model of P1-P3 that fixes 12.3's defects, models
requests as messages, and is fidelity-checked the same way before any counted run.

**What stays as it was:** Y9, Y10, W-1, W-2, W-3, Y6c-2 and E32 are not accepted. D-02, R-23 and O-FLOOR stay open.
The assumptions Codex named (W-1, W-2, AS-3, premise P, D-02 outside the model, bounded witness and reconciliation
abstractions) stand; nothing here proves any of them.
