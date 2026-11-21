# AFR-001 L-1: exclusive authority across a degrading shrink and R7 case 1

Status: **PROPOSED, for Codex and the owner.** Part A of Codex's #179 review, section 8
(`docs/handoff/PR179_Codex_Review.md`). L-1 stays **UNRESOLVED** until Codex and the owner accept a correction and
its counted verification passes. Nothing here changes a frozen or accepted text. No C++, Rust, wire-format change or
run on the R640. Claims are labelled FACT, EXT, INF or HYP.

## 1. Result

- **L-1 is real, and it breaks Claim L** (at most one authority holder). It is not a data-loss exception: G-35 and
  R7 keep their meaning, and an overlap of authority stays outside every accepted exception.
- **Root cause.** `p` changes which authority it needs when it **learns** that its degrading shrink was chosen. R7
  case 1 relies on what is **chosen**. Between the two, R reads "degraded and READY" while `p` still serves on normal
  authority, which R7 case 1 does not fence.
- **Correction A-1 (proposed).** There are two parts:
  - From the moment `p` sends a request that would make R degraded, `p` serves only under degraded authority (R8:
    grants from every member of A **and** from a majority of witnesses under W1).
  - `p` stays in that mode until it learns that a version later than the request's base is chosen.

  Every request `p` sends to change R names its base version and can be chosen only as the next version, and `p`
  has at most one such request outstanding. "Would make R degraded" is judged against that base. Binding only the
  degrading request is not enough: two rebased non-degrading shrinks, each chosen on the other's result, can leave
  S = {p} with no degrading request ever sent (found by an independent review of this package).
- **Bounded check, uncounted** [FACT, this PR, scratch runs]:
  - With A-1 off, TLC finds the overlap in 9 steps.
  - Each of the two other parts, removed alone, also yields an overlap.
  - The corrected model shows no overlap at the bounds tried. Those runs were sizing, not preregistered, so they are
    not evidence. The counted runs in section 6 wait for the owner.

## 2. The exact counterexample

The model is `spec/tla/L1Authority.tla`, the authority part of `Combined.tla`. Three nodes are used: `n1` is primary
at term 1 with S = A = {n1, n2}, `n3` is outside S, D = 2, and `OpOutsideDeg` (R7 case 1) is on.

TLC with `FixA1 = FALSE` gives this shortest trace (BFS, MaxTerm 2, MaxVer 5):

| # | Step | State after it |
|---|---|---|
| 1 | Tick | time passes |
| 2 | `ShrinkSend(n1, n2)` | `n1` asks the witnesses to drop `n2`; the result would be S = {n1}, degraded |
| 3 | `ShrinkChosen(n1)` | R at term 1 reads S = {n1}, degraded, READY. The reply to `n1` is lost, so `n1` still runs in normal mode |
| 4 | `Promote(n3)` | R7 case 1 applies (outside S, degraded, READY), so R moves to term 2 with `n3` as primary |
| 5 | `Learn(n3)` | `n3` starts its `learned + W` barrier |
| 6-7 | Tick, Tick | the barrier ends |
| 8 | `WGrant(n3)` | `n3` holds degraded witness authority |
| 9 | `Grant(n2, n1)` | `n2` has not learned term 2, and `n1` still counts it in A, so `n1` holds normal authority |

At step 9, `Auth(n1)` and `Auth(n3)` hold together, which violates `OneAuthority`.

In real time this is the trace from `docs/AFR_001_LIVE_GRANT_PROPOSAL.md` 5.1: `n3` serves from 2.2, and `n2`'s
grant at 2.0 carries `n1` to about 3.8.

**Why no existing rule stops it** [INF]:
- R3's barrier fences only grants from members that learn the new term. `n2` does not have to learn it.
- R8 fences only witness authority, and `n1` is not using any.
- R1.2 stops `n1` from dropping `n2` from A early, which is the safe direction for durability. It does nothing for
  authority.
- R7 case 1's premise, "the degraded primary has taken over", is false here: R is degraded, but `n1` has not taken
  over.

**Which promotions can overlap** [INF]:
- Automatic promotion and R2′ promotion name a member `j ∈ S ⊆ A`. `j` stops granting once it learns, so R3 fences
  `p`.
- R7 case 2 fences every member of S physically.
- Operator promotion inside S (`OpInside`) names a member of A, so R3 fences it too.

So only a promotion of a node outside A while R reads degraded is exposed, and that is R7 case 1.

## 3. Correction A-1

### 3.1 Rule text (proposed; it would be a new R8.0, nothing else is reworded)

> **R8.0 Degraded from the send.** Every request `p` sends to change R names its base version `v`, can be chosen
> only as version `v + 1`, and is `p`'s only outstanding change until `p` reads a version above `v` chosen. When the
> request would leave S = {p} as applied to version `v`, `p` records `v`. From then on, `p` serves only under degraded authority: grants from every member of A, as before, and
> from a majority of witnesses under W1. It stays in that mode until a linearizable read shows that some version
> above `v` is chosen. Then it uses the mode recorded in the latest chosen version. If the read shows only `v`, `p`
> proposes a version-only change at `v + 1` (as in LG-5) and reads again.

### 3.2 Answers to Codex's questions

| Question | Answer |
|---|---|
| Must `p` quiesce while a shrink is unresolved? | **Yes, unless it already holds witness authority.** From the send, valid authority needs witness grants. Before a witness majority has accepted the degraded version, W1 gives no grant, so `p` serves nothing: no acknowledgements, reads, standby leases or READY (R3's meaning of "serving"). "Switching immediately to witness authority" is not assumed: it may not exist yet, and then `p` simply does not serve |
| When can it acquire degraded witness authority? | When a majority of witnesses grant under W1, which each witness can do only after it has accepted the degraded version at `p`'s term (R8). A witness could carry the grant in its ACCEPTED reply, so the quiet window is about one witness round trip [HYP: that piggyback is not in today's wire format] |
| How does the R3 barrier exclude overlapping authority? | See 3.3 |
| Delayed shrink replies | A lost or late reply changes nothing. `p` leaves the mode only on a read that shows a version above `v` chosen, never on a reply. Reply-loss steps are in every trace of section 2 |
| Repeated CAS attempts, and several changes in flight | `p` has one change outstanding. Retries of it all name the same base `v`, so they are attempts in one Paxos instance, and at most one value is chosen at `v + 1`. A retry on a new base after `v + 1` was decided is a new send, and the rule starts again |
| A delayed request chosen after `p` has resumed | Impossible once a version above `v` is chosen, because the request names `v`. Today's DEGRADE request does not name its base [FACT, `tools/snc_witness.cpp:423-512`: it carries the space uuid and term, and `propose` re-reads the base and re-applies the change on every attempt, `tools/paxos.h:323-345`]. Without the binding, TLC finds the overlap again (`mVer`, section 5) |
| A "NO" reply or a timeout | Not a decision. Minority acceptances can still be adopted at `v + 1` by a later ballot [FACT, `tools/paxos.h:262-305`: phase 1 adopts the highest accepted value]. `p` stays in degraded mode until it reads a version above `v` |
| Witness grants before the shrink is chosen | Possible in real Paxos: a majority can have the degraded value as its highest accepted while another value is still chosen at `v + 1` later, with mixed ballots [INF]. That is safe, because during the pending window `p` still needs the grants of every member of A as well (3.3) |
| Operator promotion | R7 case 1 needs a chosen degraded record. That is chosen only after `p`'s send, so from then on `p` needs witness grants, which R8 fences. Inside S (`OpInside`), the promoted node is in A, which R3 fences |
| Primary crash | A restarted `p` never serves its old term again (R6.0), so pending state is not needed after a restart. A request already sent may still be chosen after the crash; it then degrades R with no live holder, which is safe. Modelled: `Restart` keeps the request in flight |

### 3.3 Why A-1 excludes overlapping authority [INF; bounded check in section 6]

Let `s` be the moment `p` sends a degrading request with base `v`. Take any promotion `P` to term `t+1`, chosen at
real time `c`, with new primary `j`. `j` serves no earlier than `learned_j + W ≥ c + d`.

1. **`P` names `j ∈ A_p`.** This covers automatic promotion, R2′, and operator promotion inside S. `p`'s authority
   needs a live grant from `j`. `j` stops granting to `t` when it persists `t+1` (R4), which happens before
   `learned_j`. Every grant `j` gave `p` was given before `learned_j`, so R3's `learned_j + W` bound already covers it.
2. **`P` names `j ∉ A_p`.** This is R7 case 1, so `P`'s base is a chosen degraded version of term `t`. In term `t`,
   only `p` proposes changes to R (R6.0, and only the current primary proposes membership changes). Each is chosen
   only on the base it names, so a chosen version is degraded only if `p` judged that request degrading when it sent
   it. So that degraded version comes from a degrading request `p` sent at some `s < c`.
   - From `s`, under A-1, `p` needs witness grants under W1 until it reads a later chosen version.
   - If that read shows a degraded mode, `p` stays degraded. If it shows a normal one, a later degraded version needs
     a new send, and the same argument applies to that send.
   - So at every moment `x ≥ c` at which `p` could serve, it relies on a majority of witness grants. R8's argument
     then applies unchanged: the majority accepting `P` meets that majority in a witness that either granted before
     accepting (grant ≤ `c`, valid to `≤ c + d`) or refuses after (W1).
3. **R7 case 2** confirms that every member of S is fenced. It does not depend on A-1.

Before `s`, R is not degraded at term `t` by `p`'s hand, so R7 case 1 cannot fire. The one gap would be a degraded
version that `p` did not send. In term `t` that needs another proposer of degrading changes, which assumption L1-AS-1
excludes.

**KIP-497 makes the same move for Kafka's ISR** [EXT, Kafka KIP-497 "Proposed Changes",
https://cwiki.apache.org/confluence/display/KAFKA/KIP-497%3A+Add+inter-broker+API+to+alter+ISR, verified]:
- On a shrink: "the leader will continue relying on the previous ISR until the LeaderAndIsr with the successful
  change has been received". The reason given: "it is not safe for the leader to assume a smaller ISR until the
  update has been successfully written".
- On an expansion: "the leader will assume the larger set immediately".
- In sum, "the leader always assumes the largest possible ISR (taking int account inflight requests)".

A-1 is that rule for authority. While a change is in flight, `p` meets the requirements of both outcomes. R1.2
already does this for A, and A-1 adds it for the witness lease.

Raft's dissertation, 4.1, is reported to say that servers use the latest configuration in their logs whether or not
it is committed [EXT, NOT VERIFIED: our fetch got only a summary of chapter 4, not its text].

### 3.4 What A-1 costs

- **No serving between the send and the witness majority's acceptance.** This is about one witness round trip if the
  grant rides on ACCEPTED (HYP), longer otherwise. It is UNMEASURED; X-3 measures degraded renewals.
- **When witnesses are unreachable after the send,** `p` stalls until it can read R. Today it would keep serving on
  normal authority, and that is exactly L-1's exposure.
- **A request that loses its instance** costs one more read, and possibly one version-only change.

## 4. Contract impact (nothing applied; each needs the owner's explicit yes)

| Text | Status | Impact |
|---|---|---|
| R8 | proposed text, D-01's leases CONDITIONALLY ACCEPTED | Adds R8.0 (3.1). W1, the holder rule, the barrier and "Leaving degraded mode" are unchanged |
| Every R-changing request (today's DEGRADE, NOTREADY, membership changes) | today's wire format | It names its base version, the witness chooses it only as base + 1, and `p` keeps one outstanding. This is a wire-format change, PROPOSED only and NOT authorized. Which of today's requests degrade R in the model's sense is a C++ mapping question, UNRESOLVED: NOTREADY also sets the witness's `degraded` field (`tools/snc_witness.cpp:460`, `:505`) |
| R1.2 | proposed | Unchanged. A drop still needs a chosen shrink, or a read at `p`'s own term. The read also tells `p` the mode, and `p` adopts it |
| R7 | proposed; case 1 and case 2 kept | Unchanged. A-1 makes case 1's premise ("the degraded primary has taken over") true by construction. Case 5c stays forbidden |
| G-35, R2′, R9-A..E, LG-1..LG-6 | various | Unchanged. L-1 stays a Claim L obligation, not a data-loss exception |
| Claim L (SA section 1), case table | proposed | Add row "3d: degrading shrink chosen, reply lost, then R7 case 1": PREVENTED by A-1 once accepted, UNRESOLVED until then |

## 5. Positive controls (each mutation alone reproduces an overlap)

Scratch runs, MaxTerm 2, MaxVer 5, MaxRestarts 1, `OpOutsideDeg` on, shortest BFS traces [FACT]:

| Mutation | Switch | Trace |
|---|---|---|
| `mA1`: the mode changes only on learning | `FixA1 = FALSE` | section 2 (9 steps after Init) |
| `mVer`: the request is rebased, as DEGRADE is today | `FixVerCAS = FALSE` | ShrinkSend, Bump, LearnOut (normal), ShrinkChosen (late, rebased), Promote(n3), Learn(n3), Tick, Tick, Grant(n2,n1), WGrant(n3) |
| `mDec`: a read showing only `v` ends the pending state | `FixDecide = FALSE` | ShrinkSend, LearnOut, ShrinkChosen, Promote(n3), Learn(n3), Tick, Tick, WGrant(n3), Grant(n2,n1) |

The model already keeps one outstanding change: `ShrinkSend`, `Grow` and `Ready` all need `~pend[p]`. So the
two-rebased-shrinks case that review B found against the earlier rule text is excluded by the model, not checked
by it. A mutation that lets two changes be outstanding at once is not modelled [UNRESOLVED as a control].

**A model defect found and fixed while building these.** `Drop` first left `pdeg` unchanged. That let `p` drop its
last member, keep normal mode with A = {p}, and hold authority with no grants. A read of R at `p`'s term shows the
mode, so `Drop` now adopts it. This is R1.2's read, not a new rule.

**A second model defect, found by an independent review.** `NewS(j)` under `R2p` gave the old primary's own re-promotion the set
S \ {prim}, which removed `j` from its own S. It now applies only when `j ≠ prim`. `spec/tla/Combined.tla:105`
(merged) has the same defect; it is recorded here and not fixed in this PR. Combined.tla's R2′ lane results stand
as recorded and should be re-checked once it is fixed [UNRESOLVED].

## 6. Minimum verification (preregistered here; counted runs need the owner's GO)

`spec/tla/L1Authority.tla` and `spec/tla/L1Authority.cfg`. Every lane uses Nodes {n1, n2, n3}, P0 = n1, Q0 = n2,
D = 2, MaxTerm 3, MaxVer 6 and MaxRestarts 1, and checks `OneAuthority`. Run L1-P checks the probe `NoL1Window`
with the same switches as L1-F. It must be violated, which shows the corrected model reaches the
shrink-then-outside-promotion window and the PASS of L1-F is not vacuous.

| Run | Switches | Expected |
|---|---|---|
| L1-F | fixes on, `OpOutsideDeg` | PASS |
| L1-Fi | fixes on, `OpOutsideDeg`, `OpInside` | PASS |
| L1-Fr | fixes on, `OpOutsideDeg`, `R2p` | PASS |
| L1-mA1, L1-mVer, L1-mDec | one fix off each, `OpOutsideDeg` | violated (the section 5 controls) |
| L1-P | fixes on, invariant `NoL1Window` | violated (reachability) |

Each run has a 2 h cap, and a timeout is NOT COMPLETED. Bounds do not change after results.

Sizing [FACT, uncounted]:
- At MaxTerm 2 and MaxVer 5, the fixed model has 743,986 distinct states and reaches depth 27 (rechecked after the
  `NewS` fix, same counts; that fix touches only `R2p`).
- A 60 s sizing launch at MaxTerm 3 and MaxVer 5 completed with no error, and its counts were not kept. It is
  disclosed here and is not evidence.

## 7. Abstractions and open assumptions (kept, not resolved)

- **L1-AS-1.** In term `t`, only `t`'s primary proposes changes that degrade R. Witness-initiated degrade does not
  exist today [INF, from `tools/snc_witness.cpp:423-512`: DEGRADE comes from a primary request].
- **L1-AS-2.** Witness Paxos is atomic, and witness grants follow the chosen record (as in `Combined.tla`).
  Per-witness W1 on the highest accepted value is argued in 3.2 and 3.3, not modelled.
- **L1-AS-3.** Time is modelled with countdown timers. The real-time argument is G-46's drift condition and R3's `W`
  on CLOCK_BOOTTIME. An untimed guard does not verify the wait.
- **L1-AS-4.** Positioning, logs and durability are left out. L-1 is an authority property, and `Combined.tla` and
  `ReportReseed.tla` cover the log side. Leaving out positioning guards only adds behaviours, so a PASS transfers
  [INF].
- **L1-AS-5.** A Grow reply loss is not modelled: Grow is atomic. Losing that reply leaves `p` degraded while R is
  normal, so `p` loses witness grants and stops, which is the safe direction [INF].
- **L1-AS-6.** Composition is not checked. `LiveGrant.tla` models the log side with its own Shrink, which is not
  version-bound and has no pending state, and this model has no logs. Neither model checks A-1 and LG-1..LG-6
  together [UNRESOLVED].
- MaxVer bounds the number of R changes in a run.
- Premise P, D-02, R-23, incarnation uniqueness and the real-time grant-expiration argument are unaffected and stay
  open.
