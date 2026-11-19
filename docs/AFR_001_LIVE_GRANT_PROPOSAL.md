# AFR-001: count only under a live grant (P1-P3), architectural revision package

Status: **PROPOSED, for Codex and the owner. Revised once after an independent two-reviewer check (section 9).** Docs only. Asked for by Codex's review of #178
(`docs/handoff/PR178_Codex_Review.md`, section 4). Nothing here is accepted. Y9-T stays **BLOCKED**. Y10, W-1, W-2,
W-3, Y6c-2 and E32 stay unaccepted, and D-02, R-23 and O-FLOOR stay open. No model is written and nothing is run. The
model plan in section 8 needs the owner's separate GO. Claims are labelled FACT, EXT, INF or HYP.

## 1. Why, in one paragraph

Every counterexample since round 5 (`docs/AFR_001_REPORT_MODEL_PREREG.md` section 12) has one root. `p` decides
from a **cached belief about `k`**: a positioning record plus the coverage it counted. `k` can invalidate that belief
by an event `p` does not see: a reseed, a wipe, a truncation after learning a term, or a request that arrives late.
Each patch (incarnation, reporter term, W-1, W-2, W-3) closed one event and left the next. This package replaces the
cache with a **promise**. `p` counts `k` only under a grant `k` gave that is still valid, and that grant names the
copy of `k`'s log it speaks for. `k` keeps that copy intact while any such grant may be valid. The existing lease
machinery (R3's `W`, on CLOCK_BOOTTIME) already bounds how long a grant can be valid. So the proof obligation moves
from "`p` has seen every event at `k`" to "`k` waited out its own promises", which `k` can check alone.

## 2. Prior art (ideas only, nothing adopted)

| Source | What it does | What we take |
|---|---|---|
| Kafka KIP-903 [EXT, https://cwiki.apache.org/confluence/display/KAFKA/KIP-903%3A+Replicas+with+stale+broker+epoch+should+not+be+allowed+to+join+the+ISR] | The motivating failure is F-1's twin. A leader proposes adding B to the ISR. B's disk is replaced and B restarts with an empty log. The stale proposal "adds B back into the ISR even though it has an empty log". The fix: "A replica with a stale broker epoch is not eligible to join the ISR". Each fetch carries the broker epoch, the ISR change names the epochs, and the controller rejects any that are not current | The identity rides on the replica's own messages, and **the decider checks it at the moment of the membership change**, not the proposer. In our P3, the witnesses' compare-and-set is that check |
| Kafka KIP-966 [EXT, https://cwiki.apache.org/confluence/spaces/KAFKA/pages/263426970/KIP-966+Eligible+Leader+Replicas] | A broker detects an unclean shutdown through its epoch. The controller "will remove the broker from ISR and ELR" before accepting its registration. Responses for an unclean election carry the broker epoch and are rejected if it no longer matches | Data loss changes identity, and a stale response from the old identity is refused |
| Viewstamped Replication Revisited, Liskov and Cowling 2012, section 4.3 [EXT, https://www.cs.princeton.edu/courses/archive/fall19/cos418/papers/vr-revisited.pdf] | A replica that lost state "cannot participate in request processing and view changes until it has a state at least as recent as when it failed", because otherwise a committed operation "might then be known to fewer than a quorum". A nonce makes it accept only responses "for this recovery and not an earlier one" | A node that discarded data counts for nothing until it has rebuilt state, and delayed messages are bound to one attempt. P1's new incarnation per discard is our nonce |
| Leases, Gray and Cheriton, "Leases: An Efficient Fault-Tolerant Mechanism for Distributed File Cache Consistency", SOSP 1989; R3's own lease bounds (`docs/AFR_001_SAFETY_ANALYSIS.md` section 0) [EXT, FACT-doc] | The grantor promises not to act against the holder until the lease expires, and the expiry is bounded by clocks with bounded drift | P2 is that promise, applied to destroying data |

Nobody we found uses the authority lease itself as the carrier of the replica's identity. Kafka uses a separate
broker epoch and a controller. We have no controller per replica, but every acknowledgement already needs a live
grant from every member of A, so the grant is the cheapest carrier [INF].

## 3. Proposed protocol text

Terms:
- `inc_k` is `k`'s incarnation.
- A **destructive action** is any step that can remove a durable entry from `k`'s log: R6.3's truncation, RS-2's
  discard, an operator wipe, and a boot that finds no node record (an empty or replaced disk).
- A no-op R6.2 round (`L` equal to `k`'s end) is not destructive.
- Discarding a torn tail above the watermark (G-14(b)) is excluded, because R9-A′ never reports past the watermark.
- `start_k` is when this process took the directory lock (contract step 1), on CLOCK_BOOTTIME.

- **LG-1. Identity per log copy** (replaces E32's definition).
  - `inc_k = (nonce, gen)`.
  - `gen` increases at every boot write and at every **marker write**: the R6.3 intent and the RS-1 record. The bump
    is part of the same record write, made before any grant or report under the new value.
  - A boot that finds no node record (fresh seed, wipe, replaced disk) draws a fresh nonce (`getrandom`) and starts
    `gen` at 1.
  - R6.3′ item 1's request number restarts at every new incarnation.
  - While a marker is set, `k` sends no report (today's R6.3′ item 3 and RESEEDING rules, unchanged).
- **LG-2. Grants and reports name the incarnation** (amends R3 and R9-A′).
  - Every authority grant carries the `inc_k` current when `k` sends it. Building a grant is serialized with setting
    `stop_k` (LG-4).
  - Every acknowledgement report carries `inc_k` and the `(term, lsn)` of the last entry the watermark covers.
- **LG-3. Count only under a live grant** (P1; replaces Y9-T's counting rule, W-2 and W-3 (a)).
  - `p` credits `m` with coverage `l` only while all three hold:
    - `p` holds a valid grant from `m` naming incarnation `g`;
    - a report from `m` names `g` and an entry `(t, l)`;
    - `p`'s log (the log it ships from) holds `(t, l)`.
  - `m`'s coverage is the largest such `l`, and 0 when no grant from `m` naming that incarnation is valid.
  - This coverage is the only input that three decisions read: "every member of A has `e`" (the acknowledgement),
    "caught up" (R1.1, Grow) and R9-B's catch-up clause.
  - `ack_p`, `inh_p`, R2′ rule 2's positioning, R9-B's other clauses, R9-C and `|S| ≥ m` are unchanged.
- **LG-4. No destruction under a live promise** (P2; generalizes RS-0's wait and W-1).
  - Before a destructive action, `k` sets `stop_k` and grants nothing from then until the action is complete **and**
    `k` has been positioned again by R6.2. That one stop covers every round of R6.2's iteration.
  - `k` writes the marker only once `W = d·(1+ρ)` has passed on CLOCK_BOOTTIME since `max(last_grant_k, start_k)`.
  - At the marker write, `k` re-makes R6.3′ item 1's checks (term, binding to its latest request, unchanged log) in
    the same step (R6.3′ item 2). If a check fails, the action is abandoned and `stop_k` is cleared.
  - A crash before the marker abandons the action; the new process waits again from its own `start_k`. A crash after
    the marker resumes the action.
  - **Operator wipe.** The wipe tool takes `k`'s directory lock after a confirmed stop and holds it until the new
    seed's boot write. It waits `W` on its own CLOCK_BOOTTIME from the later of the stop and its own start, then runs
    LG-5, then deletes.
  - **A boot with no node record** waits `W` from `start_k` and runs LG-5 before it grants or sends any request.
  - Media loss while a grant from `k` is valid is a fault of the device, not an action. It is outside the contract,
    as in any replicated system (FM, D-02).
- **LG-5. Leave S before reseeding** (P3; replaces W-3 (b)).
  - **When:** after LG-4's wait completes, while `k` still grants nothing, and before RS-1, a wipe tool's delete, or
    a fresh node's first grant.
  - **What:** `k` (or the tool on its behalf) proposes a compare-and-set on R from its current version `v` to `v+1`
    that changes nothing but the version.
  - **Proceed only if** the value chosen at `v+1` is `k`'s own version-only proposal, and it shows `k ∉ S`.
  - If another value is chosen at `v+1` (Paxos may adopt a partially accepted Grow), `k` re-reads R.
    - If `k ∈ S`, `k` waits, reading R without bumping, until a shrink removes it.
    - Otherwise it retries.
  - R6.3's truncation needs no LG-5 (section 5, C7).
- **LG-6. Append continuity** (states what I2 already assumes; reviewer finding F1).
  - Every shipped batch carries the `(term, lsn)` of the entry before its first.
  - `k` appends a batch only if that entry is its own last entry.
  - `p` ends its positioning record of `k` when a request from `k` names another incarnation (R6.3′ item 1, answering
    side, unchanged).
- **R1.1 change.** `p` adds `c` to A only after an R6.2 round has positioned `c` (reviewer finding F5a). Otherwise a
  member of A that must truncate would stop granting and suspend `p`'s authority for `W`.

Section 6 lists what this removes.

## 4. Messages and state transitions

### 4.1 Messages (changes only)

| Message | Today / Y9-T | Under LG |
|---|---|---|
| authority grant `k → p` | `(t, p)`, holder-side validity from `p`'s request send time (section 0) | adds `inc_k` |
| acknowledgement report `m → p` | Y9-T: entry `(term, lsn)`, incarnation, reporter's term | entry `(term, lsn)` and `inc_m`; **reporter's term dropped** (HYP-redundant, checked by N11) |
| shipped batch `p → k` | not specified | carries the previous entry's `(term, lsn)` (LG-6) |
| R6.2 request and reply | bound by incarnation and request number (R6.3′ item 1) | unchanged |
| version-only CAS on R | none | new: proposed by a member, a fresh node or a wipe tool; same Paxos path as any CAS |

Sizes [HYP]: the incarnation is a 64-bit nonce and a 64-bit generation, so 16 bytes per grant and per report. The
report already carries the entry's `(term, lsn)` under Y9-T.

### 4.2 State at `k` (grantor and reporter)

| State | Durable | Changes |
|---|---|---|
| `inc_k` | yes (node record) | LG-1, in the same write as the boot or marker record |
| `last_grant_k`, `start_k` | memory, CLOCK_BOOTTIME | `last_grant_k` set before a grant is sent (R3); `start_k` at the lock |
| `stop_k` | memory | set before the wait; cleared when the action is abandoned, or when `k` is positioned after it |
| marker (intent `(t, L)` or RESEEDING) | yes | written after the wait (LG-4) and, for a reseed, after LG-5 |

Transitions of `k` for a destructive action:
1. `NORMAL → STOPPED`: `k` stops granting.
2. `→ WAITED`: `W` has passed since `max(last_grant_k, start_k)`.
3. [reseed or wipe only] `→ LEFT`: LG-5 chose `k`'s version-only proposal with `k ∉ S`.
4. `→ MARKED`: the marker write, with `gen + 1` and R6.3′'s checks re-made.
5. `→ DISCARDED`: the removal is durable.
6. `→ POSITIONED`: the marker is cleared (intent cleared, or RS-6), and an R6.2 round positions `k`. `stop_k` clears;
   back to `NORMAL`.

Crashes and other events:
- A crash in `STOPPED`, `WAITED` or `LEFT` returns to `NORMAL` with a new `start_k`.
- A crash in `MARKED` or later resumes from `MARKED` after the boot write.
- **Promotion during the wait** (reviewer finding 6). A node in `STOPPED` or `WAITED` is promotable only if it is in
  S, as today. If it is promoted, it abandons the action: a primary never truncates its own log (R6.0).
- **A term change during the wait** abandons the action. `k` persists the term (R4) and starts again from R6.2.

### 4.3 State at `p` (the primary)

For each member `m`, `p` keeps:
- the grant it holds, `g_m`, valid until `s + d·(1-ρ)` on `p`'s clock, where `s` is the send time of the request
  `g_m` answered;
- the reports from `m` naming `g_m`;
- the derived coverage (LG-3);
- its positioning record of `m`. That record decides where `p` ships from and feeds R2′ rule 2. It no longer feeds
  coverage (W-2 is gone).

## 5. Protection against F-1, F-2, F-3, H-1, and the cases tried

**The claim [INF].** Assume no media faults (D-02), no reuse of an incarnation (section 6, residual premises), and
LG-6. If `p` credits `m` with coverage `l` at time `x`, then `m` durably holds `p`'s log through `l` at `x`.

**Argument.**
1. When `m` sent the report, it held the reported entry under the watermark (R9-A′). By I2, which needs LG-6, it
   held `p`'s whole prefix through `l`.
2. `inc_m` was `g` both when `m` sent the report and when it sent the grant `p` holds. Incarnations never repeat
   (LG-1), so no marker was written between those two sends, whichever came first. Every removal of a durable
   reported entry passes through a marker (LG-4's list; the reviewers checked every path: torn tail, RS-2's restart
   under the marker, recovery steps 8 and 9a, checkpoint compaction).
3. After the later of the two sends, a marker needs `W` since `max(last_grant_m, start_m)` (LG-4).
   - `last_grant_m` is at or after the grant `p` holds, and `start_m` is later than any grant of an earlier process.
   - The holder's validity ends within `d` real seconds of `p`'s request, and so of the grant.
   - `W ≥ d` real seconds, so the grant expired before any marker (section 0's waiter/holder argument, unchanged).

So no entry through `l` was removed before `x`. Acknowledgement soundness (G-11 at the time of the ack) follows,
because the ack needs coverage ≥ `i` for every member of A. Two reviewers checked this argument by hand and found it
sound for the intended reading, once findings F1-F3 were fixed (section 9).

| Case | Without LG | With LG |
|---|---|---|
| **F-1 shape 1**, a Grow on coverage counted before RS-1 | coverage outlived the reseed | coverage needs a live grant naming the old incarnation. None exists after the wait, and RS-1 takes a new incarnation |
| **F-1 shape 2**, a Grow chosen between RS-0 and RS-1 | `k` entered S, then discarded | LG-5, after the wait: either the Grow is chosen at `v+1` (`k` sees `k ∈ S` and does not reseed) or `k`'s bump is chosen (the Grow's CAS at `v` fails). **Codex's item 6** (a later Grow attempt) needs coverage under a live grant, and `k` grants nothing until it is positioned after RS-6. Its coverage then reflects the copied log |
| **The KIP-903 case**, an empty or replaced disk at boot | a pending Grow could be chosen with `k` empty | the boot finds no node record: a fresh nonce, the wait, then LG-5 before any grant |
| **F-2**, R7 outside S, truncation inside a live grant | `n2` truncated while `n1` still held its grant | LG-4: `n2` writes its intent only after its grant to `n1` has expired. Section 5.1 |
| **F-3**, a wipe seeded from a deposed primary | `n2` kept its record of the old `n3` | the seed has a fresh nonce; its grants name it, so the old report is not credited (LG-3) |
| **H-1**, a delayed R6.2 request | positioning was restored under an old incarnation, and that positioning fed coverage | coverage no longer reads positioning. A positioning restored by a late request makes `p` ship from the wrong place, and LG-6 makes `k` reject that batch (finding F1's hole at 81..100) |
| **C1**, truncation keeps the incarnation | a report before an R6.2 truncation and a grant after it name the same incarnation | LG-1 bumps `gen` at the intent write |
| **C2**, a process crash and restart without a reboot (reviewer F2) | the wait counted from the machine boot | the wait counts from `start_k`, the lock time of this process |
| **C3**, a delayed grant | — | holder validity starts at `p`'s request send time, so a late delivery only shortens it |
| **C4**, a grant to an older-term primary still valid | — | `last_grant_k` covers grants to any primary (R3) |
| **C5**, a deposed primary rejoining | — | it is a member like any other: LG-4 applies |
| **C6**, an unclean crash (volatile tail lost) | — | reports cover only watermarked entries, so nothing reported is lost. Torn or lost newest flushes are D-02's (excluded) |
| **C7**, a Grow racing a truncation | — | truncation needs a newer term, whose version is above the Grow's `v`, so the Grow fails. A same-term truncation removes only entries absent from `p`'s log, and `p` credits only entries in it (I3) |
| **C8**, a standby lease to a reseeding node | the model let SGrant run after RS-0 | R9-B's catch-up clause reads LG-3 coverage, 0 without a live grant |
| **C9**, a stale report from an earlier term, same incarnation | Y9-T needed the reporter's term | a removal between the report and the grant needs a marker, which changes the incarnation. The entry check plus I2 then fixes the prefix. HYP; N11 checks it. If a `gen` bump were ever lost, the term check would catch C1 independently, so N2 and N11 interact |
| **C10**, a supervisor restarting the node during a wipe wait (reviewer F3) | — | the tool holds the directory lock from the stop until the seed's boot write |
| **C11**, Paxos adopting a partially accepted Grow at `v+1` (reviewer F6) | — | LG-5 proceeds only if its own proposal is chosen |

### 5.1 F-2 and G-35, and a second gap the review found

**The acknowledgement half [INF].** F-2 is an additional violation, not inside the exception. G-35 says an R7
promotion outside S "can lose acknowledged writes", and AckedPreserved excepts entries acknowledged in a term below
the R7 term. In F-2, `n1` acknowledges entry 2 **after** the promotion, while it holds the only copy. That breaks
G-11's copies "at the time of the acknowledgement", which no operator acknowledgement names.

Under LG-4, `n2` keeps entry 2 while `n1` can still count it, so the acknowledgement is sound. The later loss, when
`n2` truncates after its grant expires, is exactly G-35's stated exception. The new model therefore keeps
AckedPreserved's exception (acknowledged in a term below `r7T`) and checks AckSound separately.

**L-1, a pre-existing Claim L gap, separate from LG [INF; reviewer A F4b, reviewer B 5; UNRESOLVED].** The trace:
- `n1` missed the reply to its own shrink of `n2`, so it keeps normal-mode authority, counting `n2`'s grants.
- R7 case 1 lets the operator promote `n3`, because the record reads degraded and READY.
- `n2` has not learned term 2, so it keeps granting to `n1`.
- Numerically: `n3` learns term 2 at 0 and serves from 2.2; `n2` grants to `n1` at 2.0, so `n1` holds authority
  until about 3.8. That is two authority holders for about 1.6 s.

Why it slips through: R7 case 1's premise, "the degraded primary has taken over", assumes `n1` uses witness
authority (R8), which W1 fences. It does not hold when `n1` has missed its shrink.

Candidate fix (not proposed for acceptance here): after proposing a shrink that removes `c`, `p` stops counting
`c`'s grants toward authority and runs on witness authority until it learns the outcome. This touches R1.2, R7 and
R8. It needs its own model and is listed as an open item, outside this package.

## 6. Changes to accepted or frozen texts, and what becomes redundant

| Text | Status | Change |
|---|---|---|
| R3 (SA R3) | conditionally accepted as AFR-D-01 (owner, 2026-11-16) | grants carry `inc_k` (LG-2). **Recorded revision needed** |
| R1.1 and "caught up" (SA R1, section 2) | proposed | `c` joins A only after it is positioned. "Caught up" is judged by LG-3 coverage at the CAS proposal. LG-5 orders a reseed after any Grow |
| R6.3 and R6.3′ items 1-2 | conditionally frozen with R2′ (#155) | the intent is written only after LG-4's wait, with item 1's checks re-made in the same step, and it takes a new `gen`. The request number restarts with each incarnation. **Recorded revision needed** |
| R9-A′ | conditionally frozen (#171, Y5) | reports carry `inc` and the entry `(term, lsn)`. **Recorded revision needed** |
| R9-B | conditionally frozen (#155) | the catch-up clause reads LG-3 coverage; "positioned" stays. **Recorded revision needed** |
| R9-C, R2′ rule 2, `m = 2` | conditionally frozen | unchanged |
| D-06 item 7 and 4.1 (incarnation = boot write generation) | conditionally accepted (2026-11-18) | the generation also moves at marker writes. **Owner decision needed** |
| D-06 4.3 (truncation intent record) | conditionally accepted | the intent record carries the `gen` bump |
| E32 (nonce, generation) | **proposed, not accepted** | superseded by LG-1 |
| R6.5 and contract 6.5 RS-0, RS-1 | proposed (Y6c-2 conditional) | RS-0's wait becomes LG-4's (counted from `start_k`, as round 6 already had), LG-5 sits before RS-1, and granting resumes only after positioning |
| R witness access (SA "Group record R") | proposed | a member, a fresh node or a wipe tool may propose a version-only CAS |
| Shipping (LG-6) | not specified anywhere we found (UNRESOLVED whether today's frames check continuity) | stated as a requirement |
| Wire formats (grant, report, batch) | no format frozen | fields added. **Not authorized:** proposal only |

**Redundant under LG** (proposed for removal once the model agrees):
- **W-1** becomes LG-4's wipe case.
- **W-2** is replaced by LG-3.
- **W-3 (a)** is LG-3, and **W-3 (b)** is LG-5.
- **Y9-T's reporter-term check** is HYP-redundant (C9), checked by N11.
- **"RESEEDING grants nothing"** is now implied by LG-4's stop.

**Not redundant:**
- **U4.** RS-1's new incarnation still ends `p`'s positioning record, which LG-6 and R2′ rule 2 use.
- Y9-T's entry check.
- The fresh nonce (B-7b).
- "No report while a marker is set".

**Residual premises.** The argument assumes each of these and does not check it:
- **OP-1:** a wipe without the tool.
- **AS-3:** the clocks and `W ≥ d`, which every destructive action now relies on.
- **Premise P:** a reseed copies a prefix of one primary's log.
- **D-02:** watermark and media faults, including media loss inside a live grant.
- **Incarnation reuse:**
  - a nonce collision;
  - R-23 metadata rollback;
  - a restore of the data directory from a snapshot or backup (same nonce, older generation);
  - `--links-generation-min` (contract section 3).

  LG-1 moves the generation more often, so this exposure is larger than under E32.

## 7. Costs

**Extra wait (P2).** LG-4 delays only a node that must discard:
- a rejoining member with a divergent suffix;
- a reseed, which already waits `W` today;
- a wipe or an empty-disk boot, which is new and costs `W`.

**How long.** The wait is at most `W` after the stop, that is, after the R6.2 reply that calls for a truncation.
Since `k` grants nothing from the stop until it is positioned, a truncating rejoin pays one `W` however many R6.2
rounds it needs.
- **In the common failover the wait overlaps R3.** The new primary `j` cannot serve before `learned_j + W`, and a
  survivor's reply arrives after `j` serves R6.2, so the survivor's wait ends at most `W` after that [INF].
- **When `m = 2` needs that survivor,** the first acknowledgement after a failover can be delayed by up to `W` beyond
  today. This is the main availability cost.
- **Under the R1.1 change, the write path gets no new wait in steady state.** A member of A is positioned, and it
  truncates only after a term change, when `p`'s authority has ended anyway.

**G-48.** Service resumes within the stated bound plus at most one `W` per destructive action on the critical path.
Repeated crashes restart the wait, as any boot already restarts R3's bound. One wait is unbounded:
- the case: a reseed or wipe of a member of S while the primary is down, which waits for a shrink;
- it is safe, but it is a G-48 exception: an operator resolves it, through R7 or a shrink;
- it is listed with G-48's open items.

The bound stays UNMEASURED (X-3).

**Witness load (P3).** One extra CAS per reseed, wipe or empty-disk boot. A primary's concurrent CAS can lose to it
and retry. While `k` waits for a shrink it only reads, so it cannot starve the shrink.

**Messages.** 16 bytes per grant and per report [HYP], plus the batch's previous-entry field. No new message on the
acknowledgement path.

## 8. Revised model plan (`spec/tla/LiveGrant.tla`), NOT AUTHORIZED TO RUN

### 8.1 Base and fixes

The base is `spec/tla/ReportReseed.tla`, with every section 12.3 defect fixed:

| 12.3 defect, or reviewer finding | Fix |
|---|---|
| requests were not messages (H-1) | R6.2 requests are records `[from, inc, term, blt, ble]` in a set, at most `MaxReqs`. They can be delivered late, duplicated or dropped, and `j` answers each against the request's own `blt, ble` |
| copies came from the durable log, and RS-6 checked that the source still serves | RS-5 and Wipe copy from the source's `ilog`. RS-6 checks only that `k`'s term is unchanged since RS-3 |
| SGrant was unchecked | `LeaseSound`: at a lease grant, the grantee durably holds `p`'s log through `max(inh_p, ack_p)`. Checked in the history at the decision |
| Mf was a no-op in lane B | as fixed in #178 (`MaxCrashes 0`) |
| `okE` read the in-memory log | stated: `p`'s log means the log it ships from (LG-3) |
| Promote, Drop and the R6.4 trigger | Promote's `rec.ready ∨ OpInside` restored; Drop as R1.2 plus R9-C; the R6.4 trigger is RS-0 for any `k ∉ S` |

**New structure.**
- `version` on R. `ProposeGrow(p, c)` records a pending CAS at `version`, and `ChooseGrow` may fire later only if R is
  still at that version. `Bump(k)` is LG-5, enabled only in `WAITED`. A pending Grow adopted at `v+1` is modelled by
  `ChooseGrow` firing first.
- `gr[k]` holds `<<p, inc>>` pairs. LG-4's wait guard is "no pair from `k` is held".
- Shipping checks LG-6.
- `AddA` needs positioning (the R1.1 change).
- A process restart without a reboot is a separate action from Crash, with the same untimed guard.

**Timing abstraction, stated openly.** Grants are untimed: a held grant may expire at any time, and the wait is the
exact guard "no grant from `k` held". So the model checks the logic of LG-1 to LG-6. It **cannot** check:
- that `W` from `max(last_grant, start)` achieves the guard (AS-3);
- C2 or C10's timing.

No probe claims to.

### 8.2 Properties, regressions and mutations

**Invariants.** All are checked in the history at the moment of the decision where marked:
- `AckSound` and `GrowSound`, at the decision;
- `LeaseSound`, at the decision;
- `AckedPreserved`, which excepts entries acknowledged in a term below `r7T`;
- `EntryUnique`, `LogMatching` and `HistoryUnique`.

**Regression traces, mandatory.** The cases are `ReportReseedTrR7`, `ReportReseedTrW`, F-1 shapes 1 and 2, C1, H-1's
shipping hole (F1), the KIP-903 case, C2 and C10. Each is rewritten for the new action signatures. For each trace:
1. **Name the breaking step in advance** (its index `b`).
2. **Run on the correct model** with deadlock checking on, plus an invariant `stp < b`. That invariant must hold, and
   the probe `stp # b - 1` must be violated, so the trace provably reaches the step before the break.
3. **Run the positive control:** the same trace under its matching mutation must violate the safety property. The
   pairs are TrR7 with N3, TrW with N1, F-1 shape 2 with N6, C1 with N2, H-1 with N16, and KIP-903 with N14.

A trace refused at an earlier step, or for an unrelated reason, fails step 2.

**Negative mutations.** Each removes one protection and is expected to violate.

| # | Removed | Expected shape | Lane |
|---|---|---|---|
| N1 | grants name no incarnation | F-3 | A |
| N2 | no `gen` bump at the R6.3 intent | C1 | B |
| N3 | no LG-4 wait before the intent | F-2 | A |
| N4 | no LG-4 wait before RS-1 | B-6b | A |
| N5 | no wait before a wipe | the W-1 shape | A |
| N6 | no LG-5 | F-1 shape 2 | A |
| N7 | no entry check | M4 | A |
| N8 | reports allowed while a marker is set | M5r (A), M5p (B) | A, B |
| N9 | no fresh nonce at a wipe | B-7b | A |
| N10 | the Grow guard ignores LG-3 (counts any report) | F-1 shape 1 | A |
| N13 | no `gen` bump at RS-1 | B-6a | A |
| N14 | LG-5 before the wait ends | F-1 shape 2 (reviewer finding 1) | A |
| N16 | no LG-6 continuity check | H-1 shipping hole | A |

**Controls, expected "not shown necessary".** These are candidates for removal:
- **N11** adds the reporter-term check back (lane B; same result as CB expected).
- **N12** lets a node grant while `stop_k` is set but no marker is written yet: expected violated, so this is not a
  removal candidate. It replaces the old "RESEEDING grants nothing" control.
- **N15** has no `gen` bump at boot (lane A). It is expected not shown necessary for safety, because the boot bump
  exists for R6.3′ request binding.

**Lanes.** Lanes A and B are as in `docs/AFR_001_REPORT_MODEL_PREREG.md` section 3, with `MaxReqs 1` in both lanes,
so that lane B reaches R6.2. Lane A already needed 29-89 minutes at smaller scopes, and these additions grow the state
space, so sizing at the counted bounds decides the fallbacks, which are fixed before any run. The launch list, caps
(2 h), budget and stop rules go into the preregistration that follows the owner's approval. A three-way fidelity check
of the `.tla` comes before any sizing.

## 9. Independent review (two reviewers, told to break it)

Reviewer A attacked the protocol and reviewer B checked completeness against Codex's list and simplicity. Neither
found a break of the core argument under the intended reading. Their findings, and what changed:

| Finding | Severity | Fixed in |
|---|---|---|
| B1: LG-5 was not ordered after the wait; a CAS before it lets a Grow at `v+1` with live coverage be chosen after RS-1 | BLOCKER | LG-5 "When"; N14 |
| B2: an empty or replaced disk at boot got neither LG-4 nor LG-5 (the KIP-903 case) | BLOCKER | destructive-action list; LG-4; case row |
| A-F2: "last boot" read as machine boot lets a restarted process truncate inside a live grant | MAJOR (BLOCKER on that reading) | `start_k`, the lock time; C2 |
| A-F1: shipping continuity unstated, so a stale positioning could leave a hole that LG-3 credits; U4 not redundant | MAJOR | LG-6; H-1 row; U4 kept |
| A-F3: the wipe tool did not keep the node stopped | MAJOR | LG-4 wipe clause; C10 |
| B3, A-F5c: when granting resumes was undefined; one `W` per R6.2 round | MAJOR | LG-4: no grant until positioned again |
| B4: positioning at `p` said "gone" yet frozen rules read it | MAJOR | 4.3 keeps it; LG-6 |
| B5, A-F4b: R7 case 1 with a missed shrink lets two primaries hold authority | MAJOR, pre-existing | L-1 in 5.1, open, outside this package |
| A-F4a: the model plan's AckedPreserved contradicted 5.1 | MAJOR | 8.2: exception kept, AckSound checked separately |
| A-F5a: a catching-up member of A that truncates suspends `p`'s authority | MAJOR | R1.1 change |
| A-F5b, A-F5d: the wait bound was wrong, and a wipe of a member of S with the primary down waits forever | MAJOR | section 7 |
| B6: promotion or a term change during the wait undefined | MAJOR | 4.2 |
| B7: statuses wrong (R3 is D-01; E32 is not accepted); rows missing | MAJOR | section 6 |
| B8: "refused = deadlock" is vacuous under `-deadlock`; traces could stall early | MAJOR | 8.2's three-step criterion |
| B9: 12.3 not fully fixed (`okE`, Mf, `LeaseSound`, lane B R6.2, timing, state space) | MAJOR | 8.1 |
| B10: mutation gaps (RS-1 gen, CAS order, boot gen); N10 restored a removed structure | MAJOR | N10 redefined; N13, N14, N15, N16 |
| A-F6: LG-5 proceeded on any chosen value; repeated bumps could starve a shrink | MINOR | LG-5; section 7 |
| A-F7: R6.3′ checks must be re-made after the wait; grants serialized with the stop | MINOR | LG-4, LG-2 |
| A-F8: incarnation reuse by rollback, restore and `--links-generation-min`; disk replacement | MINOR | residual premises; LG-4 |
| B11, B12: simplicity notes, sizes, a citation, no-op R6.2 | MINOR | sections 3, 4.1, 2 |

**Checked and not broken:** both reviewers found these hold.
- Steps 2 and 3 of the argument, given the fixes.
- C7 and C9.
- Grants to several primaries, and delayed grants.
- The redundancy of W-1, W-2 and W-3.

**UNRESOLVED:**
- whether today's frames check continuity (LG-6);
- whether R's compare-and-set is full Paxos with value adoption (LG-5 is written for that case and is safe either way);
- L-1;
- liveness under repeated term changes during a wait.

The fixes in this section are **not re-reviewed**.

## 10. What the owner is asked

1. **Direction:** pursue LG-1 to LG-6 and the R1.1 change in place of Y9-T, W-1, W-2 and W-3 (recommended yes). The
   texts this changes are listed in section 6. They include conditionally frozen R9-A′, R9-B and R6.3′,
   conditionally accepted R3 (D-01) and D-06 item 7, and the wire format. A yes is to the direction only: each
   recorded revision comes back with the model's evidence.
2. **The model:** write and preregister `LiveGrant.tla` per section 8, fidelity-check it, then come back for the GO on
   counted runs (recommended yes).
3. **L-1** (two primaries under R7 case 1 after a missed shrink): open it as its own item, with its own model
   (recommended yes). It is outside this package.
