# AFR-001: Y9, what makes an acknowledgement report countable (PROPOSED)

Status: **PROPOSED, for Codex and owner review.** Docs only. Y9 is a proposed change to frozen text (R9-A′, OD-10)
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
- A reseed: the replica left S first (R2′ rule 3), RS-0 drops its leases, and U4 changes its incarnation.
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
  compare-and-set, because the shrink changed R's version.
- `p` restarts and reuses LSNs (R6.0, new term): an old `(t, x)` against new `(t + 1, x)` does not match.
- Promotion: a new primary counts only reports that match its own log, so reports addressed to the old primary
  count only for entries the new primary also holds.

**Y9-B: an explicit binding** of each report to `(p's term, p's incarnation, the replica's incarnation, a positioning
number)`, as R6.3′ does for replies. It closes the same cases as Y9-T, plus the reseed residue. It costs state on
both sides and a new rule for every positioning. Not needed for safety under the cases above [INF].

## 3. Recommendation

**Y9-T, plus the sender rule that R6.3′ item 3 and R6.5 already imply.** Proposed text, replacing the report clause
of R9-A′:

> Every replica (a member of A, or one catching up to join it) reports an entry as acknowledged only once it has
> applied it and its durable watermark (D-02) covers it, whether or not it holds a lease at that moment, and never
> while it has a pending truncation intent or the incomplete flag is set. **A report names the entry by its
> `(term, lsn)`. p counts a report only if p's own log holds that entry, and then counts it as covering `lsn`.** p
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

**Cost:** one term field per report (8 bytes), and a lookup of `p`'s entry term at `lsn`. `p` keeps its term history
`H_p`, so the lookup is `term_e ∈ H_p` together with `end_p(term_e) ≥ lsn ≥ start_p(term_e)` [INF]. No new state.

**What a yes changes:** R9-A′'s report clause (frozen, OD-10); G-30 (A) in meaning; the replication `Ack` frame (a
D-06/wire format item, not authorized).

**UNRESOLVED:**
- Premise P (B9), for reporters that copied their log under R6.5.
- Whether a reporter must also have persisted `p`'s term (the model has `T[m] = T[p]`; no break found without it).
- No model checks Y9-T. `Log.tla`'s `Ack` compares entries by identity (`dlog[m][i] = dlog[p][i]`), which is Y9-T's
  rule written as an oracle, so the models assumed it.
- Degraded mode with `m = 1`.
- Whether the reseed residue (a stale report counted for a member of A outside S) matters for anything beyond
  section 0's sentence.
