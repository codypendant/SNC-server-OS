# AFR-001 LiveGrant model: specification and preregistration

Status: **PROPOSED, for Codex and the owner.** This is Part B of Codex's #179 review, section 8
(`docs/handoff/PR179_Codex_Review.md`).

What exists so far:
- The model, `spec/tla/LiveGrant.tla`, is written.
- Twelve regression traces with positive controls are written. Each passes its four-step check.
- An uncounted smoke and sizing pass was run.

What has not been run: **no counted verification.** The runs in section 7 wait for the owner's separate GO.

The status of the surrounding items is unchanged:
- LG-1 to LG-6 and the R1.1 change are design directions (OD-15, `docs/handoff/PR179_Owner_Decisions.md`), not
  accepted texts.
- Y9-T stays BLOCKED. L-1 stays UNRESOLVED (`docs/AFR_001_L1_AUTHORITY.md`).
- W-1, W-2, W-3 and the Y9-T checks stay until the model and the full safety argument show they are redundant.

Claims are labelled FACT, EXT, INF or HYP.

## 1. Result so far

- **The model is written.** It covers Codex's ten items (section 3), the 12.3 defects are fixed (section 4), and
  the timing abstraction is stated openly (section 5).
- **Writing it, smoke-testing it and reviewing it found four protocol gaps.** All four are in the design text, not
  only in the model. Section 2 has each one, its trace and a proposed rule:
  - **LG-F1.** A read of R does not decide a Grow that is still in flight, so R1.2 can drop a member that is about
    to enter S. This is the same root as L-1's `mDec` control.
  - **LG-F2.** LG-3's admission is vacuous when nothing needs covering (`max(inh_p, ack_p) = 0`). A stopped node
    can be grown back into S after its LG-5 bump, promoted, and then reseed while serving.
  - **LG-F3.** After a replaced disk, a Grow proposed before the replacement can still be chosen before the boot's
    LG-5. The empty node then sits in S. This is safe only if the node itself refuses to become primary, and grants
    nothing, until LG-5 shows it outside S, and only if its "booted with no record" state survives a crash. Text
    4.2 says the opposite for stopped nodes.
  - **LG-F4.** R7 case 1 lets the operator promote a node outside S that is about to reseed, or already is. The
    operator cannot see that. The node must refuse to become primary while reseeding (R6.5, enforced at the node),
    and abandon a reseed it has not started when it learns it is primary.
- **Every one of the twelve regression traces passes its check** [FACT, scratch runs and the repo runner,
  uncounted]:
  - the correct model satisfies the trace's property and refuses the trace at its named break step;
  - the trace reaches the step before;
  - the matching mutation violates that property, and not before the break.

  The two FACT traces from #178 (F-2, F-3) are among them, rewritten for the new actions. F-3 now needs a delayed
  R6.2 request, because R2′ rule 2 makes a member grant only once positioned.
- **Smoke** [FACT, uncounted]: random simulation of the revised model found no violation (section 8). That proves
  nothing, because random simulation also missed F-2 and F-3.
- **Lanes changed after sizing.** The three lanes first written here do not complete in hours. Section 7 now has
  smaller feature lanes, chosen from the sizing in section 8 before any counted run.

## 2. Protocol gaps found while writing the model

### LG-F1: a read does not decide a pending Grow (R1.2)

Shortest form, from smoke:
1. `p` proposes Grow(c) at version `v`. `p` adds `c` to its view of S (FD-1).
2. `p` reads R and still sees `v`, with `c ∉ S`.
3. `p` takes `c` out of its view, drops `c` from A, and acknowledges entry 1 without `c`.
4. The Grow is chosen at `v + 1`.

`c` is now in S without entry 1, which violates `AckedPreserved`. Trace: `LiveGrantTrRead`, with the mutation
switch `FixRead`.

**Rule (proposed, R1.2 addition).** A read that shows version `v` does not decide `p`'s own request based on `v`.
`p` treats `c` as possibly in S until a version above that request's base is chosen.

This is L-1's rule for degrading requests (`docs/AFR_001_L1_AUTHORITY.md` 3.1), applied to Grow. Today's
`read_linear` blocks lower ballots but not a later attempt by a witness that still holds the request
[FACT, `tools/paxos.h:244-310`, `:323-345`].

### LG-F2: admission at need 0 is vacuous (LG-3)

1. `k` leaves S by its own LG-5 bump.
2. With `max(inh_p, ack_p) = 0`, `k`'s coverage of 0 meets "caught up", so `p` grows `k` back into S. `k` grants
   nothing at this point.
3. `k` is promoted, runs RS-1 before it learns the promotion, and serves while reseeding.

This violates `AckSound`. Trace: `LiveGrantTrAdm`, switch `FixAdmGrant`.

**Rule (proposed, LG-3 addition).** Admission to S (Grow) also needs a live grant from `c`, whatever the coverage
required. With it, LG-4's stop keeps a node that has left S out of S until it is positioned again.

### LG-F3: a replaced disk with a Grow in flight (LG-4, case 4.2)

1. A Grow of `k` is proposed while `k` holds data and a live grant.
2. The grant expires.
3. `k`'s disk is replaced.
4. The Grow is chosen before `k` boots and runs LG-5.

`k` is now in S with an empty log. Nothing that `k` does at boot can prevent this, because the choice happened
before the boot. Kafka closes the same race at the decider, which checks the broker epoch (KIP-903) [EXT, cited in
`docs/AFR_001_LIVE_GRANT_PROPOSAL.md` section 2].

What the model shows [FACT, lane R, uncounted]:
- `GrowSound` fails in lane R (expected; `LiveGrantTrKIP903` shows it, and so does run PR).
- `Core` holds only if the node refuses for itself. The first draft put the rule in `Promote`'s guard, which reads
  the node's state at the witnesses' CAS, state no witness or operator can see (independent review A, finding 1).
  Now `Promote` has no such guard, and `Learn` refuses: a node with `lg = 6`, reseeding, or not yet booted does not
  become primary (`Becomes`, switch `FixRefuse`). `LiveGrantTrFresh` shows `Core` violated without it.
- The refusal needs the "booted with no record" state to survive a crash. Without that (switch `FixFreshDur`), a
  crash and restart forget it and the empty node becomes primary (`LiveGrantTrFreshCrash`, `Core` violated). No
  text says this today.

So the gap costs availability, not acknowledged data: `p` must shrink `k`, a sole-copy `p` cannot fail over to `k`,
and a promotion of `k` leaves the group without a primary until another promotion. It is still a copy lost in S.

**Rule (proposed, 4.2 correction).**
- A node that booted with no record writes that fact durably before its first boot record, and keeps it across
  restarts until it has been positioned after LG-5.
- Until then it refuses to become primary on learning a promotion, grants nothing, and does not count as a copy.
- Proposal 4.2's "a node in STOPPED or WAITED is promotable only if it is in S" stays true only for truncation
  waits, where nothing has been discarded yet.

### LG-F4: R7 case 1 can promote a node that is leaving S for a reseed (R6.5)

Found by independent review A (finding 2). In the first draft, `OpOutside` needed `lg = 0` and no reseed. That is
state the operator cannot see:
1. `n1` shrinks `n3`, then `n2`, so S = {`n1`}.
2. `n3` starts RS-0 and leaves by its LG-5 bump.
3. The operator promotes `n3` under R7 case 1.
4. `n3` runs RS-1 before it learns the promotion, then learns it.

Without a rule at the node, `n3` serves while reseeding. Its acknowledgements are unsound, and RS-2 then removes an
acknowledged entry of its own term. Trace: `LiveGrantTrOpLeave`, switch `FixRefuse`.

**Rule (proposed, R6.5 at the node).** A node that is reseeding refuses to become primary when it learns a
promotion. A node that has left S but not started RS-1 abandons the reseed and may serve; that is R7 case 1's
accepted loss (G-35), nothing more. The model's `OutsideS` allows exactly this window: a node that has left S may
be the sole member only as R7 case 1's promoted node that has not taken over.

The operator's choice of such a node is still a liveness problem, unmodelled.

**Open alternative, not proposed:** Kafka's fix, where S names incarnations and the witnesses check them. That is a
larger change to R.

## 3. Codex's ten required items

| # | Item | Where in `spec/tla/LiveGrant.tla` |
|---|---|---|
| 1 | ReportReseed fidelity defects corrected | section 4 |
| 2 | R6.2 requests delayed, duplicated, reorderable | `req` is a set of request records. `ReqSend`, `ReqDrop` and `Answer(j, q)` answer any request in it, at any time, more than once, against the request's own `blt, ble`. Counted lanes keep one request in flight (delayed, duplicated, answered after its sender changed); two reordered requests are covered by `TrW` (MaxReqs 2) only |
| 3 | grant incarnation identity and expiration | `gr[k]` holds `<<p, inc>>` pairs, `Grant` names `incn[k]`, and `Expire` removes any pair at any time |
| 4 | witness-version CAS and racing Grow | `rec.ver`. Every change to R goes through `Chg`, which bumps the version and kills pending Grows. `ProposeGrow` records a CAS at `ver`, and `ChooseGrow` applies it only at that version. One Grow is outstanding, and it stays adoptable until a later version is chosen: there is no action that loses it (the first draft's `LoseGrow` deleted an attempt that Paxos could still adopt, independent review B). Paxos adopting a partly accepted Grow at `v + 1` is `ChooseGrow` firing first |
| 5 | destructive truncation, reseed, wipe | `RecvRep` → `MarkTrunc` → `ApplyIntent`; `RS0`, `Leave`, `RS1`-`RS6`; `WipeStop`, `Leave`, `WipeDo`; `Replace` (lanes LR and LRC, a disk replaced with no tool) |
| 6 | promotion inside and outside S | `Promote` (in S; `rec.ready ∨ OpInside`; R2′ new S) and `OpOutside` (R7 case 1). Neither reads the promoted node's local state. The node's own refusal is in `Learn` (`Becomes`) |
| 7 | AckSound, GrowSound, LeaseSound, AckedPreserved, log identity | section 6 |
| 8 | earlier traces as mandatory regressions | section 6.2: twelve traces, including #178's F-2 and F-3 |
| 9 | positive controls | each trace's matching mutation, and the mutation table in 6.3 |
| 10 | three-way fidelity review of the actual `.tla` | section 9 |

LG-4 and LG-5 obligations, item by item:

| Obligation | Where |
|---|---|
| applies to R6.2 truncation, RS-2, wipe and a replaced disk | `MarkTrunc`, `RS1`, `WipeDo` and `Leave` from a replaced disk (`lg = 6`) each need `Waited(k)` |
| a Grow chosen before the bump is observed | `Leave` is one CAS that needs `k ∉ rec.S`. If the Grow was chosen first, `Leave` is disabled and `k` waits |
| a Grow on an obsolete version cannot commit afterwards | `ChooseGrow` needs `rec.ver = x.ver`, and `Chg` clears `gp` |
| a new Grow after the bump is not admitted on stale coverage or expired grants | LG-3 coverage needs a live grant naming the report's incarnation, plus LG-F2's admission grant. `k` grants nothing while `stopG` |
| no reseed while in S | invariant `OutsideS` |
| a failed or conflicting CAS changes nothing destructive | `Leave` advances the state only when its own CAS is chosen. A destructive step needs that state |
| the whole check-then-act boundary | `OutsideS` and `GrowSound` are checked in every state, not just at the version read |

## 4. ReportReseed defects (12.3) and how each is fixed

| Defect | Fix |
|---|---|
| requests were not messages (H-1) | item 2 above |
| RS-5 and Wipe copied the source's durable log | `RS5` and `WipeDo` copy the source's `ilog` (the log it ships from) |
| RS-6 checked that the source still serves | `RS6` checks only that `k`'s term is unchanged since RS-3 (`rst`) |
| SGrant unchecked | `LeaseSound`, checked at the grant. `SGrant` also needs R9-B's member conditions: `m` has persisted `p`'s term, has no pending intent, is not reseeding or stopped, and is up. A deposed primary may still grant, as the text allows |
| Mf a no-op in lane B | the generator's fallbacks differ per lane (section 7) |
| Promote dropped `rec.ready ∨ OpInside` | restored |
| Drop's `p ∉ gr[c]` was model-added | removed. `Drop` is R1.2 plus R9-C (`lease[c] # T[p]`) |
| the R6.4 trigger | stated: `RS0` for any `k ∉ S`, as before |
| `okE` read the in-memory log | stated: `p`'s log is the log it ships from (LG-3) |
| positioning fed coverage (W-2) | coverage reads only grants, reports and `p`'s log (LG-3). Positioning (`pinc`) feeds R2′ rule 2 (`Ack`) and R1.1 (`AddA`) |
| shipping read the member's log | `Ship(p, m, i)` may send any entry `i` of `p`'s log, which covers repeated, reordered and misplaced batches. `m` appends only on LG-6's predecessor match |
| Grant did not need positioning | `Grant` needs `pos[k] = T[k]`, R2′ rule 2 [FACT-doc, `docs/AFR_001_R2_AND_SINGLE_COPY.md` R2′ rule 2] |

## 5. Timing abstraction (stated before any run)

Grants are untimed. A held grant may expire at any step (`Expire`), and LG-4's wait is the exact guard `Waited(k)`:
no pair from `k` is held. So the model checks the logic of LG-1 to LG-6.

It **cannot** check:
- that `W = d·(1+ρ)` from `max(last_grant_k, start_k)` on CLOCK_BOOTTIME achieves that guard (AS-3);
- C2 (a process restart without a reboot). In the model a crash and a process restart are the same action;
- C10 (a supervisor restarting the node during a wipe wait). The tool's lock is `lg ∈ {4, 5}`, with the process
  down.

No probe claims otherwise. The real-time argument stays in `docs/AFR_001_LIVE_GRANT_PROPOSAL.md` section 5, step 3,
and G-46.

## 6. Properties, traces and mutations

### 6.1 Invariants

Each one is checked in every state. Each decision is recorded at its own step into `viol`.

| Name | Meaning |
|---|---|
| `AckSound` | at an acknowledgement, every member of A durably holds the entry, has no pending intent that would remove it, and is not reseeding, and at least two nodes hold it that way (R6.3′ item 3) |
| `GrowSound` | when a Grow is chosen, `c` durably holds `p`'s log through `max(inh_p, ack_p)` as it was at the proposal, and is not reseeding. Vacuous if that prefix is empty |
| `LeaseSound` | the same test at a standby lease grant (R9-B) |
| `PromoSound` | when a node becomes primary (at `Learn`), it holds every acknowledged entry of a term at or after `r7T` |
| `AckedPreserved` | every member of S holds every acknowledged entry of a term at or after `r7T` (the R7 exception of G-35) |
| `OutsideS` | a node that has left S for a reseed or wipe (`lg ∈ {2, 5}`, or `rs`) is not in S, except as R7 case 1's promoted node that has not taken over (LG-F4) |
| `EntryUnique`, `LogMatching`, `HistoryUnique` | log identity (G-15) |

- `Core` is the conjunction without `GrowSound` and `AckedPreserved`.
- `Safety` is all of them.
- Lanes LR and LRC check `Core`, because LG-F3 breaks the other two by design.

**Probes** must each be violated, which shows the path they name is reached:
- `NoRejectByInc`: a report is refused because its grant names another incarnation.
- `NoLateRequest`: an R6.2 request is answered after its sender changed incarnation.
- `NoBumpKill`: an LG-5 bump kills a pending Grow.
- `NoWaitBlock`: a truncation waits on a live grant.
- `NoShortReseed`, `NoWipe`, `NoCrashInReseed`, `NoAckAfterR7`.

### 6.2 Regression traces (mandatory, each with its positive control)

The files are `spec/tla/LiveGrantTr*.tla` and `.cfg`, and the runner is `spec/tla/live_grant_traces.sh`. Each
trace is a fixed sequence of the model's own actions, so it is a behaviour of the full model. Traces run at their
own bounds, larger than the counted lanes': trace lane A is 3 nodes, MaxTerm 2, MaxIds 2, MaxVer 6, with crashes,
reseeds, wipes and messages set per trace; trace lane R adds `Repl`. A step written
`Learn(n3) /\ isPrim'[n3]` asks for the version of `Learn` in which `n3` becomes primary.

Each trace has a property `P`: `Safety`, or `Core` for the lane R traces, where `GrowSound` fails by design. It
passes when all four checks hold:
1. On the correct model, `P` holds and `NoPast` (`stp ≤ b`) holds, so the trace never passes its break step.
2. `NotAt` (`stp # b`) is violated, so the trace does reach the break step.
3. Under the matching mutation, `P` is violated.
4. Under the same mutation, `EarlySafe` (`stp ≤ b ⇒ P`) holds, so the violation comes only after the break
   step.

A trace refused earlier than `b`, or for another reason, fails check 2.

| Trace | Shape | Break step `b` (0-based), the rule that refuses it | Mutation | Result [FACT, uncounted] |
|---|---|---|---|---|
| `TrR7` | F-2 (#178 `ReportReseedTrR7`): R7 outside S, `n2` truncates inside its grant to `n1` | 20, `MarkTrunc(n2)`, LG-4 wait | `FixWaitT` (N3) | 4/4 |
| `TrW` | F-3 (#178 `ReportReseedTrW`) plus a late pre-wipe R6.2 request that restores `n2`'s record of `n3` under the old incarnation | 24, `Ack(n2, 1)`, LG-3 grant names the incarnation | `FixGrantInc` (N1) | 4/4 |
| `TrF1a` | F-1 shape 1: a Grow on a report counted before RS-1, with a fresh grant after RS-6 (from review A's `XF`) | 25, `ProposeGrow`, LG-3 coverage alone | `FixCovGrow` (N10a) | 4/4 |
| `TrF1b` | F-1 shape 2: a Grow proposed before RS-0 and chosen after RS-1 | 18, `ChooseGrow`, LG-5 bump | `FixLG5` (N6) | 4/4 |
| `TrB6b` | B-6b: RS-1 inside a live grant (from smoke, minimized; the lease to a stopped node removed, review A finding 6) | 7, `Leave(n2)`, LG-4 wait | `FixWaitR` (N4) | 4/4 |
| `TrH1` | H-1's shipping hole: entry 2 shipped to an empty member | 4, `Ship(n1, n2, 2)`, LG-6 | `FixCont` (N16) | 4/4 |
| `TrKIP903` | a replaced disk with a Grow in flight, the bump first. It shows only `GrowSound` (review A finding 3); `TrFresh` is the `Core` control | 17, `ChooseGrow`, LG-5 at boot | `FixLG5` (N6), lane R | 4/4 |
| `TrRead` | LG-F1 | 4, `LearnS(n1)`, LG-F1 rule | `FixRead` (N17) | 4/4 |
| `TrAdm` | LG-F2 | 4, `ProposeGrow(n1, n2)`, LG-F2 rule | `FixAdmGrant` (N18) | 4/4 |
| `TrFresh` | LG-F3: the Grow chosen after the disk is replaced, then `n3` promoted (review A's `XK3`), `P = Core` | 18, `n3` becomes primary, refusal at the node | `FixRefuse` (N19), lane R | 4/4 |
| `TrFreshCrash` | LG-F3 with a crash and restart in between (review A's `XK2`), `P = Core` | 20, the same | `FixFreshDur` (N20), lane R with one crash | 4/4 |
| `TrOpLeave` | LG-F4: R7 case 1 names a node that left S and has started RS-1 (review A's `XO`) | 6, `n3` becomes primary, refusal at the node | `FixRefuse` (N19) | 4/4 |

Traces from the proposal's list that are not built:
- **C1** (truncation keeps the incarnation, N2) needs three terms: a stale term-1 report counted by the same node,
  promoted again in term 3. It belongs in lane B and is left to the counted N2B run, not hand-built [UNRESOLVED,
  stated before any run].
- **C2 and C10** are timing only (section 5).

### 6.3 Mutations and controls

Each mutation turns off one switch. Expected results are [INF] until run. `Safety` and `Core` are checked as their
separate conjuncts, so each run names the conjunct that failed; where a trace exists, its failing conjunct is the
expected one, and a different conjunct is reported as found, not as a match.

| # | Switch off | Removes | Expected | Lane |
|---|---|---|---|---|
| N1 | `FixGrantInc` | grants name no incarnation (counts against the positioning record) | violated, `TrW` | LW |
| N2B | `FixGenI` | no `gen` bump at the R6.3 intent | violated [HYP: C1 needs three terms] | LB |
| N3 | `FixWaitT` | no LG-4 wait before the intent | violated, `TrR7` | LT |
| N4 | `FixWaitR` | no wait on the reseed path | violated, `TrB6b` | LS |
| N5 | `FixWaitW` | no wait before a wipe or a replaced-disk bump | violated (W-1 shape) | LW |
| N6 | `FixLG5` | a plain read in place of the bump | violated, `TrF1b` | LS |
| N7 | `FixEntry` | no entry check | violated (M4) | LT |
| N8p / N8pB | `FixSendP` | reports while an intent is pending | violated (M5p) | LT, LB |
| N8r | `FixSendR` | reports while reseeding | violated (M5r) | LS |
| N9 | `FixNonce` | no fresh nonce at a wipe | violated (B-7b) | LW |
| N10a | `FixCovGrow` | Grow counts any report, admission grant kept | violated, `TrF1a` | LS |
| N12 | `FixStop` | grants while stopped | violated: a grant between the bump and RS-1 lets a Grow be proposed at `v + 1` and chosen after RS-1 | LS |
| N13 | `FixGenR` | no `gen` bump at RS-1 | violated (B-6a) | LS |
| N14 | `FixOrder` | LG-5 before the wait ends | violated (proposal finding B1) | LS |
| N16 | `FixCont` | no LG-6 check | violated, `TrH1` | LT |
| N17 | `FixRead` | LG-F1 | violated, `TrRead` | LT |
| N18 | `FixAdmGrant` | LG-F2 | violated, `TrAdm` | LS |
| N19 | `FixRefuse` | the node's own refusal (LG-F3, LG-F4) | violated, `TrOpLeave`; `Core` violated in lane R, `TrFresh` | LR (and the traces) |
| N20 | `FixFreshDur` | "booted with no record" lost at a crash | `Core` violated, `TrFreshCrash` | LRC |

**Controls**, expected "not shown necessary". These are the only candidates for removing a rule:
- **N11 / N11B** (`AddTerm`) adds the reporter-term check back. If no difference shows, Y9-T's reporter-term check
  is a removal candidate in the model, nothing more.
- **N15** (`FixGenB`) removes the `gen` bump at boot. It is expected not shown necessary for safety; the bump serves
  R6.3′ request binding.

W-1, W-2 and W-3 are not switches. They are replaced by structure (LG-3, LG-4, LG-5), so the model alone cannot
show them redundant. A redundancy claim needs the full safety argument as well, as Codex asked.

## 7. Lanes, bounds and runs (preregistered; no counted run before the owner's GO)

The setup is as in `spec/tla/README.md`: `tla2tools.jar` (sha256 there), Java 21, `-deadlock -Xmx4g`, one worker,
at most three runs at a time. `states/` is deleted after every run, and the disk is checked before each launch. The
configs are printed by `spec/tla/live_grant_cfg.sh <run>`, which refuses unknown names.

**Why the lanes changed.** The first lanes (A: 3 nodes with every feature; B: 4 nodes, 3 terms; R) do not complete:
lane A had 4.8 million distinct states at depth 13 with the queue still growing (section 8). So each lane now turns
on one feature, at the smallest bounds that still express it.

Every lane: MaxTerm 2, MaxIds 1, MaxReps 1, MaxMsgs 1, MaxGot 1, MaxReqs 1, MaxVer 4, three nodes, unless stated.

| Lane | Feature turned on | Differs from the base |
|---|---|---|
| LT | R6.2 truncation only | none |
| LS | reseed | MaxReseeds 1 |
| LW | wipe | MaxWipes 1 |
| LC | crash and restart | MaxCrashes 1 |
| LR | replaced disk | `Repl`, MaxWipes 1. Checks `Core` |
| LRC | replaced disk with a crash | as LR, MaxCrashes 1 |
| LSC | reseed with a crash (probe P7 only) | MaxReseeds 1, MaxCrashes 1 |
| LI | operator promotion inside S | `OpInside` |
| LB | four nodes, three terms (C1, N2B) | 4 nodes, `SYMMETRY`, MaxTerm 3 |

What this loses, stated before any run [INF]:
- With one entry, shapes that need two entries (for example `TrR7`'s) are not in any lane. The traces cover those
  shapes; a lane PASS does not.
- No lane combines two features (a wipe and a reseed, a crash during a wipe, and so on). Section 3's combined
  obligations are covered only where a trace covers them.

| Runs | Lane | Invariant | Expected |
|---|---|---|---|
| C-LT, C-LS, C-LW, C-LC, C-LI, C-LB | as named | `Safety` | holds |
| C-LR | LR | `Core` | holds |
| PR | LR | `GrowSound` | violated (LG-F3) |
| N3, N7, N8p, N16, N17 | LT | `Safety` | violated |
| N4, N6, N8r, N10a, N12, N13, N14, N18 | LS | `Safety` | violated |
| N1, N5, N9 | LW | `Safety` | violated |
| N19 | LR | `Core` | violated |
| N20 | LRC | `Core` | violated |
| N2B, N8pB | LB | `Safety` | violated [HYP for N2B: C1 needs three terms] |
| N11 (LT), N11B (LB), N15 (LC) | as named | `Safety` | holds (controls) |
| P4, P8 (LT); P3, P5 (LS); P1, P2, P6 (LW); P7 (LSC); P9 (LB); P10 (LR) | as named | each probe | violated |
| the twelve traces | as in 6.2 | as in 6.2 | 4/4 each |

That is 40 model-checking launches plus the trace runner. The earlier lane A run N2 (`FixGenI` in a lane where C1
cannot occur) is dropped: it could only hold.

**Fallback, fixed now; sizing does not choose it.** A run that reaches the 2 h cap without completing is NOT
COMPLETED. It is then run once more with `CONSTRAINT Shallow` (every behaviour of at most 20 states; run name
with a `d` suffix, as printed by `live_grant_cfg.sh`). If that also reaches the cap, it is run once more with the
bound at 16 (`CONSTRAINT Shallow16`, suffix `e`). Each result is reported as the depth-bounded check it
is, with its depth. If the depth-16 run also reaches the cap, the run stays NOT COMPLETED. Both depths are below the
longest traces (up to 27 steps), which stay covered only by the traces. The ladder was fixed from the depths the
sizing reached (section 8), not from any result.

**Mutation and probe timeout rule.** A mutation or probe run expected to be violated that ends without a
violation, completed or not, is reported as NOT SHOWN, never as evidence that the rule is unneeded. Only a completed
run that holds supports a removal candidate (the controls).

**Stop rules:**
- Each run is capped at 2 h. A timeout counts as NOT COMPLETED, never as a pass.
- No bound, switch or invariant changes after a counted result.
- If any C- run or C-LR fails, the campaign stops. The counterexample goes to Codex and the owner, as in #178.
- An unexpected mutation result is reported as found, not explained away.

**Cost estimate** [INF, from section 8]: up to 40 runs, each up to three 2 h attempts (unbounded, depth 20, depth
16), three at a time, is at most about four days of machine time. Most mutation and probe runs are expected to end
within minutes, so the real cost is mostly the eight correct-model runs and the controls.

## 8. Sizing

All sizing is uncounted: 4-core scratch machine, 15 GB, one TLC worker per run, up to four runs at once. A sizing
run is not evidence for anything but size. Where a sizing run completed, that is disclosed below and is still not
a result.

**First draft, the original lanes** [FACT]:
- Lane A (MaxReqs 1): 4.8 million distinct states at depth 13, 2.8 million on the queue, still growing.
- Lane B: 2.6 million distinct at depth 12, still growing.
- Feature splits at the original bounds (MaxIds 2, MaxVer 6), about 4 minutes each: 0.87 to 1.05 million distinct,
  depth 14, growing. Reduced variants (MaxVer 4, MaxReps 1, MaxGot 1, ...) still grew at about 300,000 distinct
  states a minute at depth 13-16.

**Revised model, the counted lanes' bounds, no depth bound** [FACT]:

| Lane | Run time | Distinct states | Queue | Last BFS level |
|---|---|---|---|---|
| LT | 10 min | 3.11 M | 0.80 M | 21 |
| LS | 10 min | 3.62 M | 1.27 M | 18 |
| LW | 10 min | 3.62 M | 1.28 M | 18 |
| LC | 4 min | 1.64 M | 0.68 M | 14 |
| LR (`Core`) | 4 min | 1.64 M | 0.65 M | 15 |
| LI | 4 min | 1.40 M | 0.42 M | 19 |
| LB | 4 min | 0.83 M | 0.38 M | 14 |

None completed. So a 2 h run may not complete either, and the fallback in section 7 is expected to be used.

**Depth-bounded at 20, 15 minutes each** [FACT]:
- LT completed in 11 min 50 s: 2,639,112 distinct states, depth 20, no error. Disclosed; not counted.
- LS reached level 19 (5.29 M distinct, queue 1.79 M), LR level 18 (5.41 M, 1.91 M), LB level 16 (2.84 M, 1.18 M).
  None completed.

**Smoke on the revised model** [FACT]: random simulation, depth 60, 4 minutes per lane at the trace bounds: about
5.1 M states in trace lane A (`Safety`), 5.3 M in trace lane R with one crash (`Core`), 4.0 M in the old lane B
(`Safety`). No violation.

In BFS, reaching level `n` means every state up to depth `n − 1` has been found, so the depth-16 step of the ladder
is within reach of every lane inside the cap [INF]. Whether depth 20 is, for LS, LR and LB, is unknown.

## 9. Three-way fidelity review

Three passes over the actual `.tla`: the author's, and two independent reviewers told to break it. Review A
compared the model with the protocol text. Review B tried to break the model and the L-1 package. Each finding was
checked by a run before it was accepted. Every finding is listed, with what was done.

**Review A (model against protocol), 3 MAJOR, 6 MINOR:**

| # | Finding | Disposition |
|---|---|---|
| A1 | MAJOR. LG-F3 was enforced by `Promote`'s guard on `lg = 6`, state the witnesses cannot see, and the crash path kept that state with no text saying so (its traces `XK3`, `XK2`) | Fixed. The node refuses at `Learn` (`FixRefuse`); the durable "no record" state is a proposed rule with its own switch (`FixFreshDur`). Traces `TrFresh`, `TrFreshCrash` |
| A2 | MAJOR. `OpOutside`'s `lg = 0` and the `~rs` guards stood in for R6.5 at the node (`XO`, `XO2`) | Fixed. Guards removed; refusal at `Learn`; LG-F4 added (section 2); `OutsideS` states the R7 window. Trace `TrOpLeave` |
| A3 | MAJOR (evidence). `TrKIP903` shows only `GrowSound` | Accepted. Relabelled; `TrFresh` is the `Core` control |
| A4 | MINOR. `TrF1a` needed two switches off, so it did not isolate LG-3 coverage | Fixed. Rebuilt from review A's `XF`, violated with `FixCovGrow` alone (N10a) |
| A5 | MINOR. A new primary set `inh` before draining its pending intent | Fixed. `inh` is set when the intent clears (R9-B, S2′) |
| A6 | MINOR. `SGrant` lacked R9-B's member conditions, and was stricter than the text for deposed primaries | Fixed (section 4). `TrB6b` relied on a lease to a stopped node and was re-minimized |
| A7 | MINOR. `AckSound` counted a member whose pending intent removes the entry | Fixed (section 6.1) |
| A8 | MINOR. Grants carry no term, and there is no R3 fence on authority grants | Kept as an abstraction (AS-6). It only adds behaviours |
| A9 | MINOR. Some guards are stronger than the text: `RS0` needs `k ∉ S`; `Replace` not during a stop or reseed; `Drop` and `AddA` need the current term | Kept, recorded (AS-7). Each removes behaviours, so a PASS does not cover them |

**Review B (break it), on both packages; the LiveGrant items:**

| # | Finding | Disposition |
|---|---|---|
| B5 | MAJOR. `LoseGrow` deleted an attempt that Paxos could still adopt | Fixed. Removed; one outstanding Grow until a later version is chosen |
| B6 | MAJOR. LG-F3 must be the node's own refusal, and an `OpInside` lane is needed | Fixed (as A1). `OpInside` is lane AI (section 7) |
| B7 | MAJOR. LG-4 at a replaced disk is vacuous: `Replace` needs `Waited` | Accepted. Stated as an assumption (AS-8), not a checked result |
| B8 | MAJOR. Sizing, fallback rule and these sections were missing | Done: sections 7 and 8 |
| B9 | MAJOR. LG-6 at floors is argued only | Accepted. It stays an unmet verification item (section 11), not a claim |
| B10 | MINOR. The admission grant should name the incarnation | Fixed: `<<p, pinc[p][c]>> \in gr[c]` |
| B11 | MINOR. Targeted mutations, N10a, a `NoReplace` probe run, a mutation-timeout rule | Done (sections 6.3 and 7) |
| B12 | MINOR. Composition with L-1: this model's Shrink is not version-bound | Recorded (AS-9 and L1-AS-6) |

Review A's trace names (`XK2`, `XK3`, `XO`, `XO2`, `XF`) refer to its scratch modules, which are not committed; the
committed traces in section 6.2 replace them.

Review B's L-1 findings (B1-B4) and their fixes are in `docs/AFR_001_L1_AUTHORITY.md` (sections 1, 2, 3.1, 5 and 6).

**Not resolved by any review:** whether a real enforcement point exists for LG-F3 and LG-F4 other than the node's
own refusal, for example KIP-903-style incarnations in S (UNRESOLVED, a protocol question).

## 10. Assumption register (everything a result depends on)

- **AS-3:** clocks and `W ≥ d` (section 5). The model checks a guard, not the wait.
- **Premise P:** a reseed or wipe copies a prefix of one primary's shipping log.
- **D-02:** media faults are outside the model.
- **AS-8. A disk is replaced only after `k`'s grants have expired.** `Replace` needs `Waited`, so LG-4 for a
  replaced disk is assumed, not checked (review B7). A replacement inside a live grant is a D-02 case.
- **R-23** and incarnation reuse: the model's nonces never repeat.
- **O-FLOOR, FL-floor:** the LG-6 identity at a compaction floor is argued only (section 11).
- Witness Paxos is atomic per version. Adoption is `ChooseGrow` first.
- Shrink is atomic and its reply is not lost here. L-1 is modelled separately in `spec/tla/L1Authority.tla`
  (AS-9).
- Bounds: MaxVer bounds changes to R. MaxReqs, MaxMsgs, MaxGot and MaxReps bound messages in flight and remembered.
- R6.3′ item 1's request number is not modelled. Binding is by incarnation, term and log end [HYP: enough, because
  a stale reply that matches all three describes the same log].
- Standby read leases have no reads. `LeaseSound` checks the grant decision only.
- **AS-6.** Authority grants carry no term, and R3's `learned + W` fence on them is not modelled. This only adds
  behaviours (review A8).
- **AS-7.** Guards stronger than the text, each removing behaviours: `RS0` needs `k ∉ S`, `Replace` is not allowed
  during a stop or reseed, `Drop` and `AddA` need the current term (review A9).
- **AS-9.** This model's `Shrink` is atomic and not version-bound, and `L1Authority.tla` has no logs. A-1 and
  LG-1..LG-6 are not checked together (review B12).
- **AS-10.** The fresh-disk rule's durable marker (LG-F3) is modelled as `lg = 6` surviving `Crash`. Its format and
  write order are not specified.

## 11. LG-6 at checkpoint floors and compaction

LG-6 compares a batch's predecessor `(t, l)` with the identity of `k`'s last entry. That identity must exist even
when the entry itself is gone. Where it comes from [FACT-doc unless marked]:

| `k`'s state | Last-entry identity | Source |
|---|---|---|
| retained records above the floor | `(term, lsn)` of the last valid record | the H pass, `docs/AFR_001_DURABILITY_CONTRACT.md` step 5 (`E = max(last valid LSN, retain_from − 1)`) |
| every record reclaimed (idle stream, header-only newest segment) | `(floor_term, retain_from − 1)` | the manifest floor and the segment header's `prev_term` (boundary rule 7) |
| no checkpoint yet | `(0, 0)` | boundary rule 6 (`retain_from = 1`, `floor_term = 0`) |
| after RS-5 / RS-6 | the copied state's floor, then any shipped records | RS-5 publishes the stream's floor before the clear (`docs/AFR_001_D02_ACCEPTANCE_REVIEW.md` RS-5 row) |
| after an R6.3 truncation to `L = retain_from − 1` | `(floor_term, L)` | contract step 2.7, boundary rule 7 |

So the check needs only the end LSN and its term, never the record body, and the predecessor need not be physically
present [INF].

| Case | What LG-6 does [INF] |
|---|---|
| ordinary batch | predecessor = `k`'s end: append |
| repeated batch (already applied, wholly or partly) | predecessor is below `k`'s end: reject; `p` resumes from `k`'s end. A rejection never truncates: only R6.3 removes entries |
| reordered batch (a later one first) | predecessor above `k`'s end: reject |
| divergent tail (same LSN, other term) | term mismatch: reject. `k` repairs only through R6.2 and R6.3 under LG-4 |
| predecessor at `retain_from − 1` | compared with `floor_term` |
| predecessor below `retain_from − 1` | cannot equal `k`'s end (the end is at least `retain_from − 1`): reject |
| `p`'s own entry at `l` reclaimed | `p` needs `term(l)` too: available for `l ≥ retain_from_p − 1` (its floor); below that `p` cannot ship and R6.4 reseeds `k` |
| recovery after compaction | the identity is recomputed from the H pass and the floor, never from a volatile tail; a crash also bumps `gen` (LG-1), so `p`'s positioning record ends and a new R6.2 round runs |

**Why a matching predecessor implies a matching prefix** [INF, by induction, the shape of Raft's Log Matching Property; EXT, Ongaro and Ousterhout 2014, not re-fetched]: every entry in `k`'s log arrived through a checked append, a reseed copy of a prefix of
one primary's log (premise P), or a seed. `(term, lsn)` never names two contents (G-15, `HistoryUnique`).

**Dependencies recorded, not resolved:**
- **O-FLOOR.** `floor_term` must be durable and correct whenever the end is at the floor. FL-floor verifies nothing
  independent at `L = retain_from − 1` (D-02 review, section 7 items 4 and 8): `floor_term` there rests on the
  record's CRC alone. A rolled-back record (R-23) can give a stale `floor_term`.
- **FL-floor and seed floor** (section 7 items 3, 5, 7): unmodelled.
- **Premise P** for the copy.
- **Today's frames** may not carry the predecessor (UNRESOLVED; no C++ checked). The model checks the rule, not any
  frame format.
- The model has no checkpoints. Its LG-6 check compares the predecessor with `k`'s last entry, which always exists
  there, so the floor rows above are argued only.

## 12. What the owner is asked

1. Approve the LG-F1 to LG-F4 rules as additions to the design direction. They are proposed, not accepted.
2. GO for the counted runs in section 7, after Codex's review of this package and its fidelity review.
