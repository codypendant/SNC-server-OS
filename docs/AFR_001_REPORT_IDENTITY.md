# AFR-001: Y9, what makes an acknowledgement report countable (PROPOSED)

Status: **PROPOSED, for Codex and owner review.** Docs only. Revised in round 6 (Codex #173 review): the report also
carries the reporter's incarnation (section 4). Revised again in round 7 (Codex #174 review): it also carries the
reporter's durable term, and the incarnation includes a node nonce (section 5). Y9 is a proposed change to frozen text (R9-A′, OD-10)
and is **not accepted**; nothing here is an owner decision. Every claim is **[INF]**, argued on paper, not
model-checked, unless marked FACT.

Asked by Codex's review of #172 (`docs/handoff/PR172_Codex_Review.md`, section 3): may a report count only from a
replica positioned on the current primary's log in the current term; should it carry `(term, LSN)`; is an explicit
identity binding needed across incarnation changes, delayed messages and promotion; reconcile with R9-B, R9-C and
leases. "Do not assume that positioning alone solves every stale-report scenario."

## 1. What a report is today [FACT]

- A standby's acknowledgement carries one number, `durable_lsn` ("the standby's WAL is durable up to here",
  `src/core/replication.h:55-59`), sent from `wal_->durable_lsn()` (`src/core/shard.cpp:2234`).
- Positioning happens once per connection: the standby's hello carries `last_crc`, the CRC of record `next_lsn − 1`
  (`replication.h:39`, `replication.cpp:667-675`), and the primary checks it against its own record
  (`replication.cpp:408-436`). Reports that follow on that connection are trusted by LSN.
- A new connection replaces the old one (`replication.cpp:697-705`). So today a report is bound to a positioning
  only by TCP: in effect, "positioning alone".

The protocol texts model messages that can be delayed and duplicated; R6.3′ binds R6.2 replies to an incarnation and
a request number for exactly that reason. A report has no such binding.

## 2. Three candidate rules, and attempts to break each

**Y9 as written in #172 ("positioning alone").** `p` counts a report only from a replica positioned on `p`'s log in
`p`'s term.

*Breaks* across a delayed message. `S = {j}` (degraded) and `m` is outside S.
1. `m`, positioned by `j` in term `t`, holds `(t, 1..5)` and sends "covered through 5".
2. `j` crashes before writing `(t, 5)` locally, which it had already shipped (`storage.cpp:651-652`, R6.0).
3. `j` re-promotes itself to `t + 1` (frozen 4.2).
4. `m` runs R6.2 and truncates to 4 (`end_j(t) = 4`), and `j` positions `m` in `t + 1`.
5. `j` writes `(t + 1, 5)`.
6. The delayed report from step 1 arrives. `m` is positioned in `t + 1`, so `j` counts it, and acknowledges
   `(t + 1, 5)` with `m` not holding it.

TCP prevents this in today's code; the protocol text does not. The same shape works with any delay that spans a
truncation. **Severity under `m = 2`** (round 5): step 6 alone acknowledges nothing at HA_DURABLE, since `|S| = 1`.
But after step 4 `m` may legally be grown into S (its end, 4, reaches `max(inh_j, ack) = 4`); if the stale report
then lands, `(t + 1, 5)` is acknowledged at HA_DURABLE while `m`, a member of S, does not hold it. That breaks I4.

**Y9-T: the report names the entry, `(term_e, lsn)`.** The report names the highest entry the replica has applied and
its watermark covers. `p` counts it only if `p`'s own log holds an entry with term `term_e` at `lsn`. It then counts
it as covering `lsn`.

*Why it is true when counted.* By I1 the entry `(term_e, lsn)` is unique, and by I2 a member that holds it agrees
with `p`'s log through `lsn`, for both members not incomplete. A report states a fact at its send time. The fact
stays true while `p` is primary with authority, because the replica loses an entry it reported only by one of these:
- R6.2 truncation. This removes only entries not common with the primary it reconciles against (I3). For `p` to
  count it toward an acknowledgement of `e`, every member of `p`'s A must report `e`, and A ⊇ S, so the newer primary
  `j′` (a member of S, outside R7) holds `e`; by I3, R6.2 against `j′` keeps `e` on the replica. [Round 5: the
  earlier sentence relied on R3 fencing `p` first, which is false: `p`'s grant can outlive the replica's term change
  by up to `d`.]
- Completing a pending intent: excluded by R6.3′ item 3, below.
- A reseed: the replica left S first (R2′ rule 3), RS-0 drops its leases, and U4 changes its incarnation, **which `p`
  sees only because the report carries it** (round 6; without that, U4 has no effect on counting, section 4).
- A media fault: within the budget, its recovery refuses. This depends on D-02 (a lost newest flush is D-02's to
  detect) and is on HOLD with it.

*Attempts to break it.*
- The positioning-alone case above: `(t, 5)` against `j`'s `(t + 1, 5)` does not match, so it is not counted.
- An unpositioned replica holding `(s, x)` where `p` holds `(t, x)`: it does not match.
- An unpositioned replica holding a common prefix: its report is true and may be counted. It holds those entries
  durably.
- A pending intent `(t_j, L)` and an entry above `L` that also matches `p` (R6.3′'s P1 shape, four nodes):
  completing the intent deletes the entry after it was counted. **Y9-T alone fails here.** R6.3′ item 3 already
  forbids counting such a node. Y9-T keeps that as a sender rule: no report while an intent is pending, or the flag
  is set.
- A report sent before an in-place reseed and arriving after it. [Round 5 broke the earlier "residue: accepted":
  with `S = {p, k}`, a shrink to `{p}` is chosen but `p` misses it; `k` grants `p` a renewal, then reseeds and
  discards; the stale report arrives, matches, and `p`, still believing `S = {p, k}` and holding `k`'s grant,
  acknowledges at HA_DURABLE with one real copy. Neither positioning alone nor Y9-B stops it.] **Closed by a reseed
  rule (contract 6.5, RS-0):** `k` stops granting and does not discard until `W` has passed since its last grant, so
  by then `p` has no authority from `k` or has dropped it from A. A Grow that would re-add `k` fails its
  compare-and-set, because the shrink changed R's version. [Round 6: this closes only grants from before the reseed,
  and only if the wait survives a crash; a later, legitimate Grow and new grants reopen it. Section 4.]
- `p` restarts and reuses LSNs (R6.0, new term): an old `(t, x)` against new `(t + 1, x)` does not match.
- Promotion: a new primary counts only reports that match its own log, so reports addressed to the old primary
  count only for entries the new primary also holds.

**Y9-B: an explicit binding** of each report to `(p's term, p's incarnation, the replica's incarnation, a positioning
number)`, as R6.3′ does for replies. It closes the same cases as Y9-T, plus the reseed residue. [Round 6: its replica-incarnation part **is**
needed (section 4); its primary term and incarnation and its positioning number are not.]

## 3. Recommendation

**Y9-T, plus the sender rule that R6.3′ item 3 and R6.5 already imply.** Proposed text, replacing the report clause
of R9-A′:

> Every replica (a member of A, or one catching up to join it) reports an entry as acknowledged only once it has
> applied it and its durable watermark (D-02) covers it, whether or not it holds a lease at that moment, and never
> while it has a pending truncation intent or the incomplete flag is set. **A report names the entry by its
> `(term, lsn)` and carries the replica's incarnation and its durable term. p counts a report only if p's own log
> holds that entry, the incarnation is the one under which p positioned that replica, and the replica's term equals
> p's term, and then counts it as covering `lsn`.** p
> counts no other report. …(the lease sentence unchanged)

Positioning stays where frozen text already requires it: R2′ rule 2 for survivors, and R9-B for leases. It is not
made a condition for counting, because the example in section 2 shows it does not close the delayed-message case,
and Y9-T does.

**Reconciled with:**
- **R9-B (leases):** "judged by the position `m` reports (R9-A′)" now means the LSN of a matching report. Positioning
  and the other conditions are unchanged.
- **`ack_p`:** "reported every entry up to it as acknowledged" means by matching reports. It moves no faster than
  before.
- **R9-C:** unchanged. Counting needs every member of A. A member whose reports stop matching (it was truncated
  under another primary) blocks acknowledgements until it is dropped. That is a liveness cost, and its bound is
  UNRESOLVED, as in `docs/AFR_001_R65_INVARIANTS.md`.
- **R9-F:** `c_m` and `ack_p` frames are unchanged.
- **R2′ rule 2 and S5:** unchanged; a positioned survivor's reports also match.
- **U1** (`docs/AFR_001_R65_INVARIANTS.md`): **kept verbatim.** For a replica that copied its log under R6.5, I2
  comes from Lemma C, which depends on premise P (B9); without P a node in U could report a matching `(t′, y)` while
  its state reflects `(s, y)`. Y9-T's soundness for such reporters depends on P.

**Cost:** an entry term, the reporter's term and an incarnation (nonce and generation) per report (32 bytes), and a lookup of `p`'s entry term at `lsn`. `p` keeps its term history
`H_p`, so the lookup is `term_e ∈ H_p` together with `end_p(term_e) ≥ lsn ≥ start_p(term_e)` [INF]. No new state.

**What a yes changes:** R9-A′'s report clause (frozen, OD-10); G-30 (A) in meaning; the replication `Ack` frame (a
D-06/wire format item, not authorized).

**UNRESOLVED:**
- Premise P (B9), for reporters that copied their log under R6.5.
- ~~Whether a reporter must also have persisted `p`'s term~~: yes (round 7, section 5).
- No model checks Y9-T. `Log.tla`'s `Ack` compares entries by identity (`dlog[m][i] = dlog[p][i]`), which is Y9-T's
  rule written as an oracle, so the models assumed it.
- Degraded mode with `m = 1`.
- Whether "caught up" (SA R1.1, the Grow condition) is judged from reports or only from the R6.2 request's end. If
  from reports, the incarnation check is needed there too (round 6).
- Whether the reseed residue (a stale report counted for a member of A outside S) matters for anything beyond
  section 0's sentence.

## 4. Round 6: what a report must carry, and the grant wait (Codex #173 review)

Codex asked whether a report must also carry an incarnation or a positioning identity, "without a demonstrated need",
and whether RS-0's wait covers every grant path. One independent reviewer, told to break it, found two BLOCKERs
[INF; not model-checked].

**B-6a: a report sent before an in-place reseed counts after `k` is grown back.** `m = 2`, term `t`.
1. S = A = {p, k}. `p` writes `(t, 1..10)`; `k` watermarks them and sends "(t, 10)", which is delayed. 7..10 are
   not yet acknowledged.
2. A shrink to {p} is chosen; `p` drops `k` from A. Clients wait on 7..10.
3. `k` is reseeded in place from `p`: the checkpoint has `max_lsn = 6`, RS-5 copies through 6, RS-6 publishes.
4. R6.2 positions `k` at 6; `k` is caught up to `max(inh_p, ack_p) = 6`, a Grow re-adds it to S at a new version,
   and `k` grants to `p` again.
5. The delayed report arrives. `(t, 10)` is in `p`'s log, so Y9-T without incarnation counts it, and `p`
   acknowledges 7..10 at HA_DURABLE while `k` lacks them. `p` crashes; `k` is promoted; 7..10 are lost. G-11 and I4
   break.

RS-0's wait covers only grants given before the reseed, and the Grow compare-and-set only `p`'s stale Grow.
**Fix:** the report carries the reporter's incarnation (D-06 item 7 with E32), and `p` counts it only under the
incarnation in which it positioned that replica. Why enough [INF]: within one incarnation a watermarked entry is lost
only by R6.2 truncation (Y9-T's A ⊇ S argument, section 2) or a media fault (D-02, on HOLD); an in-place reseed
always takes a new incarnation at RS-1 (U4) and a restart a new boot write. A positioning number is not needed: a new
positioning within one incarnation removes no common entry (I3). [Round 7: the next sentence is false; see section 5.] `p`'s
term in the report is not needed: Y9-T's entry term already rejects a report across a term change.

**B-6b: RS-0's wait does not survive a crash of `k`.** The last grant's time is in memory only (SA, R3), and
recovery with RESEEDING resumes at RS-2 (contract 2.2 step 11). `k` grants `p` a renewal, writes RS-1, crashes and
restarts within `W`, and discards at once; a delayed pre-reseed report, still under the old incarnation that `p`
holds as positioned, then counts with a still-valid grant and one real copy. The incarnation check does not help:
`p` has not seen the new incarnation yet. **Fix (contract 6.5 RS-0):** `k` writes RS-1 only once `W` has passed since
the later of its last grant and its current boot, so RESEEDING on disk implies the wait is done; a node whose record
says RESEEDING grants nothing; granting resumes only after RS-6 and R6.2 in the new incarnation (round 6: RS-0 named
no end).

**Codex's grant paths, checked [INF]:**

| Path | Covered by |
|---|---|
| initial grant, renewal, a grant in flight at RS-0 | `last_grant` is set before the reply is sent; the holder's validity `d(1−ρ)` ends within `d` real seconds, and `W = d(1+ρ)` lasts at least `d` |
| a grant to a newer-term primary, or to one `k` has not heard from | RS-0 stops every grant; the wait covers all of them |
| a grant by an earlier process of `k` (crash during RS-0..RS-2) | the wait runs from the later of last grant and boot, and precedes RS-1 (B-6b) |
| a grant after the reseed | incarnation check (B-6a) |
| delayed or duplicated reports within one incarnation | stay true (I3; Y9-T section 2) |
| a report across a term change and truncation | Y9-T's entry term |
| promotion | a new primary counts only entries its own log holds, and only under its own positioning's incarnation |
| R9-B, `ack_p`, lease eligibility | a counted report is a true fact about the incarnation `p` positioned, so R9-B's "position `m` reports" and `ack_p` move no faster than before; R9-C unchanged |

[Round 7 found B-7a and B-7b, section 5.] No path below two watermarked copies was found beyond B-6a and B-6b, under premise P and with D-02's media-fault
exception. **Today's code** is not exposed: a new connection replaces the old (`replication.cpp:697-705` [FACT]), so
TCP binds a report to one positioning; the protocol text did not.

## 5. Round 7: the reporter's term, and a reusable incarnation (Codex #174 review)

One independent reviewer re-checked round 6's two fixes, told to break them [INF; not model-checked].

**B-7a (MAJOR): a stale report counts across two term changes within one incarnation.** Four nodes, `m = 2`, no
reboots, so `k` keeps incarnation `i`.
1. Term 1: S = {p, j′}; `k` and `j″` are in A, catching up. `p` writes `(1, 1..10)`; `k` and `j″` watermark them,
   `j′` holds 1..6. `k` sends "(1, 10), inc i", delayed. 7..10 are not acknowledged.
2. `p` crashes; `j′` is promoted to term 2, degraded (S = {j′}). `k` runs R6.2 against `j′`, truncates to 6, and is
   positioned by `j′` in term 2.
3. `j′` is lost; the operator promotes `j″` (R7 case 1, degraded and READY), which holds `(1, 1..10)`: term 3.
4. `k` runs R6.2 against `j″` and is positioned at 6, still incarnation `i`.
5. The delayed report reaches `j″`: `(1, 10)` is in its log and the incarnation matches, so `j″` counts `k` as
   covering 10 while `k` holds 6. R9-B grants `k` a lease past what it holds, `ack_p` reaches 10, and if "caught up"
   is judged from reports `k` is grown into S and 7..10 become HA_DURABLE with one real copy.

Round 6's I3 argument holds only against the primary that positioned `k`; here `k` was truncated against `j′` and
counted by `j″`. **Fix:** the report carries `k`'s durable term at send, and `p` counts it only if that term equals
`p`'s term. Within one (term, incarnation) every positioning is against the term's one primary, so I3 applies.
Whether this reaches G-11 depends on whether acknowledgements in terms after an R7 promotion outside S fall under I4
(SA lists "R7 outside S" as an exception): UNRESOLVED; the field is cheap either way.

**B-7b (MAJOR): the incarnation is not unique across a wipe and a fresh seed.** D-06 4.1 says the incarnation "is
unique and grows with every boot", but init and seed start the generation again (D-06 4.1's record writes), so a node
that is wiped and seeded (an operator reseed after a refusal, or E14′'s slot-epoch case) can reach the same
incarnation number again, and a delayed report from before the wipe matches entry and incarnation: B-6a again.
**Fix:** the incarnation is the pair (a 64-bit node nonce drawn with `getrandom` at every init and seed, the
generation), compared for equality. R-23's metadata rollback could still reuse a generation; that stays with R-23.

**Wording (MINOR):** RS-0's "grants again only after RS-6 and R6.2" was not enforceable after a crash between RS-6
and R6.2 (the record then says NONE). It is not needed: RS-3's request ended `p`'s positioning of the old
incarnation (U4), and RS-3 persisted any newer term (R4), so a grant before positioning counts no stale report. RS-0
now says so. "Current boot" meant machine boot to one reader; it now means "since this process took the directory
lock", measured on `CLOCK_BOOTTIME`.

**Checked, not broken:** delayed renewals (a request arriving after RS-0 is refused; one in flight is bounded by the
holder's send-time anchor); crashes between RS-0 and RS-1 (the lock-time wait covers the earlier process); crashes
after RS-1 (RESEEDING grants nothing); an ordinary reboot (old-incarnation reports stay true until the new R6.2,
then are rejected); a primary restart (its records are volatile and R6.0 forces a new term); lease eligibility and
Grow (covered by the incarnation and term checks); liveness under `m = 2` (no deadlock found).

## 6. Gaps found while planning the model (Codex #175)

Planning the bounded model (`docs/AFR_001_REPORT_MODEL_PREREG.md`, sections 6 and 8) found two more items [INF],
proposed only:
- **W-2.** Y9-T checks identity only when a report is counted. A coverage counted before a reseed would survive the
  end of the positioning record. It would then count again once `k` grants after RS-6, with no delayed message
  involved. Proposed text: "`p`'s counted coverage of `m` belongs to its positioning record of `m` and ends with it;
  it also ends when `p` learns a newer term."
- **W-1.** An operator wipe within `d` of the node's process stopping lets `p` count a pre-wipe report under a grant
  that is still valid, whatever the nonce. There is no rule yet. The candidate is that a fresh seed waits `W` after
  taking the lock before its first R6.2 request.

The same review showed that the entry-term check and the sender rule are both needed. A report sent while
unpositioned can pass the term and incarnation checks. Completing an intent changes neither the term nor the
incarnation.
