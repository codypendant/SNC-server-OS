# Fail-closed migration serving (D1/D7) — design for review

**Status: IMPLEMENTED (the D1/D7 slice authorized by the PR #39 review,
2026-11-04 17:45 UTC), pending review.** Sections 0-19 are the design as
approved; section 20 records what was built, where it differs, the
measurements and the residual risks. Repair (section 11), R08, R11 and S1
are not built.
Work order: cody's review of PR #38, 2026-11-04 17:22 UTC.

**The question.** How does SNC decide that a slot involved in a committed
or in-progress migration has enough local migration evidence to be served?

**The principle (from the work order).**

- Placement ownership is necessary, but it is not sufficient to serve a
  slot when the placement history requires evidence that a migration
  completed.
- When that evidence is missing, the slot fails **closed**, not open.

**What is out of scope.**

- R08: promotion eligibility. This slice does not change who may be
  promoted.
- R11.
- S1, S2 and S3 of MIGRATION_STAGING.md.
- Repair of a slot that has failed closed. Section 11 describes it, but
  it belongs to a later slice.

**Evidence labels.** The same as MIGRATION_STAGING.md:

- **CODE:** read from the source at 88bc897 (main after PR #38), cited as `file:line`.
- **INFERRED:** reasoned from the code, not run.
- **MEASURED:** observed in a run. Only section 20 holds measurements;
  sections 0-19 are the design as approved (their CODE cites are 88bc897).

## 0. Summary and decisions for review

**Findings.** The causal paths in sections 1 and 2 include three D1
variants beyond the one reported in PR #38:

- **D1b.** A STAGED record left behind by an *aborted* earlier move is
  activated by a later commit of a different move. Activation checks only
  the epoch, not the move ID.
- **D1c.** An ACTIVE record from an *earlier ownership era* of the same
  slot is served after a lossy promotion.
- **D1d.** A SEALED record from an earlier era is reopened by the
  driver's own UNSEAL, and the pre-move image is served.

Also, **lossy promotion has two forms**: the in-place `PROMOTE`, and
restarting a standby's data directory without `--standby` (documented at
`snc_server.cpp:27-29`). The second form bypasses `Engine::promote`.

All of them need an external fact that no node holds locally: **which move
made the current owner the owner**. The placement does not keep that fact
after the move record is cleared.

**Proposal.** The proposal has five parts.

1. **Arrival record in the placement.** For each slot the placement keeps
   `arrived[S] = (move_id, epoch)`: the commit that last changed S's group.
   - It is absent for a slot that has never moved.
   - It is bounded: one entry per moved slot, at most 4,096, each
     overwritten by the next commit.
   - It is the only format change. It needs placement encoding `v2`.
     The witnesses store the placement body as an opaque value
     (`snc_witness.cpp:164`), so they need no change.
2. **A serving proof.** It is checked wherever `serving()` is checked
   today (section 4). A slot that arrived by move M is served only from
   local evidence of M itself:
   - ACTIVE(M);
   - STAGED(M), activated first; or
   - a record of a later move that this group sealed and then aborted.

   Nothing else counts as evidence: not phase NONE, not an older ACTIVE,
   and not an older STAGED.
3. **A serving barrier at every start that may hold a replica prefix
   (D7).** This covers an in-place promotion, and a start of a data
   directory that was last a standby. Before such a node serves anything,
   it:
   - reads the placement linearizably;
   - adopts that map;
   - re-seals any slot that the placement shows moving away from its group
     at SEALED or STAGED;
   - waits for the re-seal to be durable.

   Until it serves, the node answers `ROLE m` (promoting). A promotion with
   `--control` already fails when the control plane cannot be reached
   (`engine.cpp:570`); this adds one `PLACEMENT SYNC` to the same step.
4. **A commit gate in the driver (D7).** The driver commits only when:
   - at least one source member answers `ROLE p`;
   - every member answering `ROLE p` reports S as not served;
   - no member answers `ROLE m`.
5. **Visibility.** A slot that is owned but unproven is not served. It is
   reported in `INFO`, `STATUS`, a log line, a trace and a counter, and
   clients get `E_MIGRATING`.

**Decisions requested:**

1. **Accept a placement format change (`v2`, the `arrived=` key).**
   Recommended. Section 15 explains why existing state is not enough.
2. **Accept repair as a later slice.** Until then, a slot that fails the
   proof stays unavailable until an operator acts, and no data is
   invented. Recommended.
3. **Client status.** Keep `E_MIGRATING` for clients, with the reason
   shown on the admin side. The alternative is a new status code, which
   is a client wire change. Recommended: keep `E_MIGRATING` in this slice.
4. **Slots moved before the upgrade** have no `arrived` entry (`Ma = 0`)
   and keep today's rule until their next move. For them D1c and D1d stay
   open. An optional backfill command can set their entries (section 15).
   Recommended: include the backfill command in the slice.
5. **A second, small non-WAL format addition:** a synced `DIR/role` file
   (`primary` or `standby`), next to the existing `DIR/node` file. It lets
   a start tell "my own disk" from "a standby's prefix". Recommended. The
   alternative is a `PLACEMENT SYNC` at every start, which loses
   availability when the control plane is down (FAILURE_MODEL C6).

## 1. D1: the exact causal path (destination)

The CODE facts behind every variant are these.

**Serving rule.** `serving(S)` holds when the map gives S to this group
and the phase is 1 or less (`shard.h:47`).

**Where the phase comes from.** It is read from the slot-state objects the
node holds:

- `refresh_slots` sets it at recovery, at promotion and on the standby
  (`shard.cpp:1256`, `:1269`, `:1394`).
- When a node has no slot-state object for S, the phase is NONE (0).

**Activation.** `activate_staged` turns STAGED into ACTIVE when two things
hold (`shard.cpp:2096-2116`):

- `group_of[S] == group`;
- `map.epoch >= st.epoch`.

It does **not** compare move IDs.

**When the destination's acks are only local.** B's acks are
LOCAL_DURABLE when its group is async (`repl_hold_` false) or degraded
(CORE_CONTRACTS section 4).

**D1: phase NONE.**

1. Move M of S from A to B. B's primary P_B writes staged records, then
   STAGED(M, E).
2. `wait_lsn` returns once these are local only: P_B's group is async or
   degraded.
3. The driver does CAS STAGED, then the commit CAS: `slots[S] = B` at
   epoch E.
4. P_B is lost before its standby Q_B receives STAGED(M). Q_B may also
   lack some of the staged records.
5. Q_B is promoted. That happens either through an async witness
   promotion (CORE_CONTRACTS E1, R08) or through an admin `PROMOTE`, which
   has no term or degraded guard (E3). `Shard::promote` runs
   `refresh_slots`. Q_B has no slot state for S, so the phase is NONE.
6. Q_B's map gives S to B: it fetches the committed map, or it already
   had it.
7. Q_B serves S with whatever image it has, partial or empty:
   - lookups get `E_NOT_FOUND` for the missing objects;
   - creates mint into S;
   - follow-ups run against the partial objects.

The same holds if Q_B is promoted by restarting its data directory without
`--standby`. That path runs `finish_recovery` and never enters
`Engine::promote`.

**D1b: a stale STAGED is activated.**

1. Move M1 of S from A to B is staged at B as STAGED(M1, E1), then
   aborted. B keeps STAGED(M1), which is inert (MIGRATION.md, Limits).
2. Later, move M3 of S from A to B. Its epoch is the current placement
   epoch plus 1, which equals E1 if nothing committed in between, or
   exceeds it.
3. M3 commits after P_B wrote STAGED(M3). P_B is lost, and Q_B holds
   STAGED(M1) and possibly part of M3's copy ahead, but not STAGED(M3).
4. At promotion, `activate_staged` finds STAGED(M1), the map's
   `group_of[S] == B`, and `epoch >= E1`. **It activates M1's copy**,
   which is old and partly overwritten by M3's copy ahead, and serves it.

**D1c: a stale ACTIVE from an earlier era.**

1. S arrived at B by M3 (ACTIVE(M3)), later left B by M5 (SEALED(M5)), and
   came back by M7.
2. B's standby is lossy: its prefix ends after ACTIVE(M3) and before
   SEALED(M5).
3. It is promoted after M7 commits. Its phase is ACTIVE, and the map gives
   S to B, so **it serves the M3-era image**.

**D1d: an earlier-era seal is reopened by UNSEAL.**

1. S starts at A (genesis), moves from A to B by M5, and comes back from B
   to A by M7.
2. A replica of A whose prefix ends at SEALED(M5) is promoted.
3. Any later driver run for S (`move` or `move --abort`) calls `MOVE
   UNSEAL` at A through `settle_owner`. `NodeHandler::unseal` checks only
   that S is A's and that no move with that ID exists
   (`migrate.h:275-280`).
4. A unseals and **serves the pre-M5 image**.

**Shared root cause.** Today a node serves when the map gives it the slot
and *nothing local says no*. **Nothing ties the local evidence to the
commit that gave the slot to this group.**

**When a restart is affected.** A node acks STAGED only after its own
fsync. A restart of a data directory that was last a **primary** therefore
holds every record it acked. A data directory that was last a
**standby** holds a replica prefix, which may be lossy, whether it is
promoted in place or restarted as a primary. The predicate applies at
every start, and the barrier of section 5 applies to every start of a
former standby.

## 2. D7: the exact causal path (source)

**Path 1: the promoted source keeps serving.**

1. Move M of S from A to B. A's primary P_A seals S as SEALED(M). The
   seal's ack waits for `durable()`, which is local only when A is async
   or degraded (`shard.h:312`).
2. The driver does CAS SEALED. Then `EXPORT final` at P_A, which needs
   P_A's seal to be durable (`shard.cpp:2248`), and STAGE final at B.
3. P_A is lost. Its standby Q_A lacks SEALED(M) and may also lack some of
   P_A's last writes.
4. Q_A is promoted. Its phase for S is the prior phase, NONE or ACTIVE,
   and its map still gives S to A, since the commit has not happened yet.
   **Q_A serves S and acks new writes W.**
5. The driver, holding B's OK, does CAS STAGED and commits S to B. B
   activates P_A's sealed image.
6. **W is lost** silently. Transfers whose debit or credit was in W stop
   conserving money. Until Q_A adopts the new map, **two groups accept
   writes for S**: Q_A, unsealed with the old map, and B. That breaks
   MIGRATION.md's primary safety property.

**Path 2: Q_A is promoted before the CAS to SEALED.**

1. The driver then does CAS SEALED using the seal OK it got from P_A.
2. `EXPORT final` at Q_A is refused, because Q_A has no seal for M.
3. The move cannot commit. It is stuck until it is aborted, which is a
   liveness problem, not a safety one.

This is safe only while P_A is dead.

**Path 3: P_A is alive but partitioned.** This is the async split-brain
case.

1. P_A is cut off from the witnesses and from Q_A, so it never learns that
   it was replaced.
2. The driver can still reach P_A. P_A answers `ROLE p` and exports its
   sealed state.
3. Meanwhile Q_A was promoted before the CAS to SEALED and serves S.
4. The commit then produces two owners, as in path 1.

Today `Driver::primary` takes the first member that answers `ROLE p`
(`migrate.h:491-506`).

**When a restart is affected.** The export needs the seal to be locally
durable, so P_A's own data directory has SEALED(M). A standby's directory
restarted as a primary is path 1 again, and the start barrier covers it
(section 5).

## 3. The current serving predicate (CODE)

```text
serving(S) = cluster == null
          || (map.group_of[S] == this group && phase[S] in {NONE, ACTIVE})
```

**Callers.**

- Executing an op (`shard.cpp:322`), and creating and deleting objects
  through `refuse_unserved` (`:429`, `:492`).
- Minting (`:172`).
- `index_find` (`:742`).
- Running a follow-up (`:1870`: held while S is owned but not served, and
  re-routed when S is foreign).
- `resume_all` (`:1288`), `OpCtx::local` (`:1717`) and the `serving`
  field of `slot_report`.

**Standby lease reads.** These use the same predicate. The phase is
updated when a slot-state record is installed (`install_indexed`,
`:1456`).

## 4. The proposed serving-proof predicate

Inputs:

- the adopted map, carrying `arrived[S] = (Ma, Ea)`, with `Ma = 0` for a
  slot that has never moved;
- the local slot state `st`, which may be absent;
- the node's role.

```text
SERVE_ALLOWED(S) =
     role is primary (or standby under a valid lease, unchanged), not withdrawn
  && map.group_of[S] == this group
  && PROVEN(S)

PROVEN(S) =
     Ma == 0 && st.phase in {NONE, ACTIVE}         // never moved, or moved before v2: today's rule
  || st.phase == NONE  && unsealed(st)  && st.move_id > Ma  // a later move away, aborted
  || st.phase == ACTIVE && !unsealed(st) && st.move_id == Ma // arrived by exactly that move
  || st.phase == ACTIVE && unsealed(st)  && st.move_id > Ma  // a later move away, aborted
  // STAGED(Ma) is not served as it is: activation first turns it into ACTIVE(Ma).
  // SEALED is never served (unchanged).
```

**Activation becomes exact.** `activate_staged` turns STAGED into ACTIVE
only when all of these hold:

- `st.move_id == Ma`;
- `group_of[S] == this group`;
- `map.epoch >= st.epoch`.

**Why "unsealed with `move_id > Ma`" is proof.** Today a seal is written
only while the slot is served (`seal_slot` refuses a foreign or a sealed
slot). Under the new rule a seal also requires PROVEN. Move IDs are
placement versions, so they grow, and a seal for a move prepared after
the commit Ma can only have been written in the Ma era, from a proven
state. A replica holds a prefix of the stream, so a replica that holds
that unsealed record also holds everything before it, including the
evidence for Ma. An unsealed record with `move_id < Ma` is from an
earlier era and **is not proof**. That is the D1c pattern.

**Never-moved NONE vs evidence-lost NONE.** The two are distinguished by
`Ma`:

- `Ma == 0`: the slot has not moved since genesis, or it was moved
  before the upgrade (section 15). Today's rule applies, so D1c and D1d
  stay open for such a slot until its first v2 commit.
- `Ma > 0` with no slot state: evidence was lost. The slot fails closed.

**Stale inputs.**

- **A map older than the local state.** Example: local ACTIVE(M7) with an
  adopted map whose `arrived` entry is M3. The proof fails, and the slot
  is refused until the map is refreshed. That matches today's behavior
  for an older map (FAILURE_MODEL C2).
- **A stale move ID or epoch** can never match `Ma` exactly.

**State size.** O(1) per slot: the existing 40-byte `SlotStateV1` plus one
placement entry. There is **no migration history** to keep.

**Cost on the hot path.** None beyond today's. PROVEN is recomputed into
the per-slot phase cache only when that slot's state or the map changes:

- `write_slot_state` and `install_indexed`, for the slot written;
- `refresh_slots` and `set_map`, as **an explicit pass over every slot the
  shard owns** (`slots_.owner[s] == id`). Slots with no state object have
  no writer today; this pass is what marks "absent state, `Ma > 0`" as
  UNPROVEN. It covers 4,096 slots across shards, a few µs, INFERRED.

`serving()` then reads a single cached byte, as it does now. The cache
holds an extra volatile value, `UNPROVEN`, which no durable record ever
stores. Each slot's entry is written only by its owning shard
(`shard.h:51-53`).

**One liveness side effect, and why it is not a safety problem.** A
sibling shard on the new map can mint an ID in S before S's own shard has
run `set_map` (`shard.cpp:172`). The create is then refused by S's shard.
That costs liveness only; nothing unsafe is served (as CORE_CONTRACTS
I-g notes for map adoption).

## 5. Source rule (D7)

**Rule.** Once a final export of G's seal for move M may exist, a source
group G must not serve S, whatever its local state.

- **Committed away** is already covered: the map gives S to another group.
- **The placement at SEALED or STAGED** does not change the map, so a
  promoted source that lost its seal needs the barrier below.
- **One exception, by design: path 2 of section 2.** A promoted Q_A may
  serve S while the placement says SEALED. This holds only if Q_A read
  the placement as PREPARED, the seal was never exported, and the move can
  no longer commit. The driver then aborts the move (c).

**(a) The serving barrier.** It runs at every start that may hold a
replica prefix:

- an in-place promotion (witness or admin `PROMOTE`);
- a start of a data directory whose `DIR/role` says `standby`, including
  one restarted without `--standby`.

In order:

1. **Before step 1 of `Engine::promote` (or before `finish_recovery`'s
   serving pass):**
   - run `PLACEMENT SYNC`, a linearizable read; if it fails, the promotion
     or start fails, as when no lease block is available;
   - from this point, answer `MOVE ROLE` with `m` (promoting), until the
     node serves.
2. **Adopt the synced map** if it is newer, with its `arrived` entries.
3. **Inside `Shard::promote`, after `repl_cfg_ = rc`** (the shard is now a
   primary and its prefix is final, after `stop_standby` and settling) and
   **before `refresh_slots`/`start_serving`**: if the move record is
   `{S, from = G, state in {SEALED, STAGED}}` and the local state of S is
   not SEALED(that move):
   - if PROVEN(S), write SEALED(M) locally, under the new term (`prior` =
     the current phase). It is an ordinary seal: every op refuses it,
     follow-ups are held, it is exportable, and only UNSEAL after a
     linearizable check undoes it;
   - if not PROVEN, write nothing. The slot already does not serve.

   For a start without `--standby`, the same check runs in
   `finish_recovery` before `start_serving`.
4. **Wait** until every re-seal is locally durable, then write
   `DIR/role = primary` (synced). Only then serve and answer `ROLE p`.

**Why it is safe.** Q_A's linearizable read happens at a time T. There are
three cases:

- **T after the CAS to SEALED** (path 1). The read sees SEALED, STAGED or
  the commit, so Q_A re-seals or treats S as foreign. It never serves.
- **T before the CAS, and P_A dead** (path 2). Q_A serves. A final export
  needs a sealed source primary. P_A cannot answer, and Q_A has no seal,
  so the move never commits. The driver aborts it (c).
- **T before the CAS, and P_A alive** (path 3). Q_A serves, and P_A can
  export. The commit gate (b) refuses to commit while Q_A answers `ROLE m`
  (still promoting) or `ROLE p` with `serving = 1`. The gate runs at
  placement STAGED, so a read Q_A makes after the gate sees STAGED, and Q_A
  re-seals.

**What the re-sealed Q_A means for the move.**

- **Placement STAGED:** B already holds P_A's complete sealed image, and
  the commit proceeds. The writes P_A acked are kept. Q_A never acked new
  writes.
- **Placement SEALED:** B's copy ahead usually holds versions from P_A that
  are newer than Q_A's. A final export from Q_A is then refused at B
  (`o->version > x.version`, `shard.cpp:2357`). This is liveness only. Two
  ways out:
  - if B already reports STAGED(M) from P_A and the gate passes, the driver
    does the CAS to STAGED without re-exporting, keeping P_A's newer image
    (a proposed driver change);
  - otherwise, abort. The abort loses P_A's unreplicated tail, which is the
    R08 loss, not a migration fault.

**(b) The driver's commit gate.**

- **When.** In `step_staged`, before the commit CAS, the driver asks every
  source member for `ROLE` and `INFO S`.
- **Condition.** It commits only if all of these hold:
  - no member answers `ROLE m`;
  - at least one member answers `ROLE p`;
  - every member answering `ROLE p` reports `serving = 0` for S.

  Either SEALED(M) or "unproven" satisfies the last condition.
- **Otherwise** it retries. If a serving `p` persists, it aborts. The
  placement is STAGED, not committed, so an abort is still allowed.

**(c) Abort when the source has no seal.** At placement SEALED, if the
source primary reports no SEALED(M), whether or not it serves S, and B
does not report STAGED(M), the driver aborts. It does not loop.

**Residual case, not closed here.** A second primary that the driver
cannot reach: async split brain with a partition. That is the HA authority
problem (R08, CORE_CONTRACTS E3, the known "cut-off primary" gap), and it
is recorded under section 16.

## 6. Destination rule (D1)

**Rule.** If the placement says B owns S by move M (`arrived[S].move ==
M`), B serves S only with ACTIVE(M), or with STAGED(M), which it
activates first. Otherwise B does not serve S:

- clients get `E_MIGRATING`;
- the admin side shows the reason (section 13).

**What fails the proof, and what does not.**

- **The same outcome for D1, D1b, D1c and D1d.** In each case the rule
  produces no serving and no activation.
- **No promotion-time step is needed beyond the barrier's map adoption.**
  The proof is a pure function of the adopted map and the local state.

**Recovery (separate from safety).** The proof failure itself repairs
nothing:

- B's partial records stay inert (section 11).
- Repair is a later slice. Source A still holds the sealed copy of S for
  M. A keeps its sealed copies (MIGRATION.md, Limits), and A's SEALED(M)
  was durable before the export.
- A repair would re-run the final stage of the same move M:
  1. HAVE at B;
  2. `EXPORT final` at A, which A still allows because it is sealed for M;
  3. STAGE final at B, allowed into an owned but unproven slot only for
     `move_id == arrived[S].move`;
  4. then STAGED(M), then activation.
- If A has also lost SEALED(M) (lossy on both sides), no complete image
  exists anywhere. The slot stays closed, and any way to accept that loss
  is a separate, explicit disaster-recovery operation. **Ordinary
  `PROMOTE` never creates one.**

## 7. Placement state and serving

| Placement for move M (S: A to B) | Source A serves S? | Destination B serves S? | Enforced by |
| --- | --- | --- | --- |
| no move; `arrived[S]` = (Ma, Ea) | iff PROVEN at A | no (foreign) | proof |
| PREPARED | iff PROVEN at A (copy ahead runs) | no (foreign) | proof |
| SEALED | **no** once a final export may exist: local SEALED(M), or re-sealed at the barrier. Exception: path 2 (a promoted Q_A that read PREPARED serves; no export exists; the driver aborts) | no (foreign) | local seal + barrier (5a) + abort (5c) |
| STAGED | **no**: as SEALED; the gate refuses a commit while any source member serves or is promoting | no (foreign; phase STAGED) | local seal + barrier + gate (5b) |
| COMMITTED (`slots[S]=B`, `arrived[S]=(M,E)`) | no (foreign) | iff ACTIVE(M), or STAGED(M) then activated | proof + exact activation |
| NONE after commit | no (foreign; A keeps SEALED(M) inert) | as COMMITTED | proof |
| NONE after abort | after UNSEAL: iff PROVEN (unsealed, `move_id > Ma`) | no (foreign); STAGED(M) at B stays inert and can never activate (`arrived[S]` is not M) | proof + existing unseal check |

**Why not "placement SEALED means never serve" as a continuous rule.**
That would need every node to adopt every move-state change. Today nodes
adopt only epoch changes (`engine.cpp:764`). The local seal already
enforces the rule for a source that sealed. Only a node holding a
replica prefix can lack it, and the barrier covers that case.

**What changes in the commit CAS.** The driver's commit sets
`slots[S] = to`, `epoch = m.epoch`, and now also `arrived[S] = (m.id,
m.epoch)`, in the same CAS. Prepare, the CAS to SEALED, the CAS to STAGED
and abort do not touch `arrived`.

## 8. Recovery, startup and promotion

| Barrier | What happens before anything serves | D1 | D7 |
| --- | --- | --- | --- |
| Process or host restart, `DIR/role = primary` (own disk) | `resolve` gets the placement (GET, or the stored copy if the control plane is unreachable). The **startup map must carry `arrived`**: `control::apply` builds `Config` from the resolved placement (`control.h:209-211`), so `Config` gains it too. `finish_recovery` runs `refresh_slots` (computing PROVEN) **before** `start_serving` and `activate_staged` (`shard.cpp:1268-1277`) | own disk holds every acked STAGED/ACTIVE, so PROVEN holds for what it legitimately owns; a stale stored map fails closed until refresh | own disk holds every acked SEALED |
| Start of a data directory with `DIR/role = standby`, without `--standby` (an offline promotion) | the barrier (5a): `PLACEMENT SYNC` (the stored map is **not** accepted), adopt, re-seal, wait durable, `role = primary`; then the serving pass | proof on the synced map | barrier |
| Witness promotion | `Engine::promote`: lease block and `PLACEMENT SYNC` (`ROLE m` from here), adopt; then `stop_standby` and settle; then re-seal inside `Shard::promote` before `refresh_slots`/`start_serving`; wait durable; `role = primary` | proof on the synced map | barrier |
| Admin `PROMOTE` | the same `Engine::promote` (SIGUSR1, `snc_server.cpp:584`, `:659-663`) | same | same |
| Placement refresh or map adoption | `set_map` (pass 1 of invariant 17) recomputes PROVEN for every owned slot **before** `activate_staged` and `release_held`. **Adoption keys on `(epoch, arrived)`, not epoch alone**: `update_placement` today adopts only `epoch > cur` (`engine.cpp:764`), and `placement_fetch` rewrites the stored file only on a newer epoch (`control.h:224`). Either the backfill bumps the epoch (proposed, so adoption is unchanged), or adoption compares placement versions | yes | n/a (epoch changes only) |
| Standby install | `install_indexed` updates the phase cache with PROVEN on the standby's map | lease reads stay safe | n/a |
| Driver restart | no node change; the driver re-reads the placement | n/a | the commit gate runs again |

**Recovery barrier (CORE_CONTRACTS section 12).** PROVEN is computed in
`refresh_slots`, which already runs before the serving pass. **No window
exists in which a slot serves and is only later found unproven.**

**One change in promotion.** The node's own map may be older than the
placement at promotion time; a lagging standby's stored map lags with its
WAL, which is why the barrier refuses the stored map. Today it is adopted later, by the watcher
thread. The synced map is now adopted before `Shard::promote`. That is
required for D7: Q_A must see a commit that already happened.

## 9. Abort

**How a source returns to serving.** A legitimately aborted move returns
the source to serving through the existing UNSEAL:

1. The driver's `settle_owner` sends `MOVE UNSEAL`.
2. `NodeHandler::unseal` reads the placement linearizably. It unseals
   only if S is this group's and no move with the seal's ID exists.
3. The unsealed record carries the seal's move ID. That ID is greater
   than `Ma`, so PROVEN holds.

**How the proof tells an abort from lost evidence.** An abort leaves a
durable **unsealed record of a move newer than the arrival**. Missing
evidence leaves NONE with `Ma > 0`, or a record older than `Ma`.

**An abort is never bricked.**

- A seal from a re-sealed promotion is cleared by the same UNSEAL.
- **D1d is closed by the same rule.** An earlier-era SEALED(M5) that
  UNSEAL reopens becomes unsealed with `move_id = M5 < Ma = M7`, which is
  not proof. The slot stays closed. `NodeHandler::unseal` needs no
  change.
- A slot that was unproven before the move stays unproven after the
  abort, because no seal was written for it. That is correct: the abort
  does not create evidence.

## 10. Repeated migrations

**The example from the work order.** A to B under move 10, B to C under
move 20, C to A under move 30:

- After move 30, `arrived[S] = (30, E30)`.
- A serves S only with ACTIVE(30), or with a record of a later move away
  from A that was then aborted.
- A's old SEALED(10) and B's SEALED(20) are inert. They never prove
  anything for 30.
- A replica of A whose prefix ends anywhere before STAGED(30) fails
  closed. A prefix holding STAGED(30) is correctly activated and served.

**Information kept.** One `(move_id, epoch)` per slot in the placement,
overwritten by each commit. Move IDs are placement versions, which grow
monotonically (`migrate.h:395`).

## 11. Partial-data cleanup

**Safety never needs cleanup.** Unproven and stale staged objects are
inert under the predicate:

- not served;
- not indexed for lookups;
- not minted into;
- not resumed.

**What happens to them.**

- **Reused** by a repair, or by the next move of S to that group. HAVE
  lists them, and the final stage replaces or drops what differs, as
  today.
- **Overwritten** by any later final stage for that slot.
- **Never activated by mistake.** Exact activation (section 4) is what
  makes leaving them safe.

**Garbage collection** of leftovers stays a general open item
(MIGRATION.md, Limits). It is not part of this slice.

## 12. Index, mint, follow-ups and other paths

**All of these read `serving()`.** With PROVEN folded into the cached
phase, every caller fails closed without being changed one by one.

| Path | Effect on an unproven or fail-closed slot | Where |
| --- | --- | --- |
| execute, create, delete | refused (`E_MIGRATING`) | `shard.cpp:322`, `:429`, `:492` |
| sequenced resend | the session's recorded answer can still be replayed. It is a stored answer of this node's own history; nothing executes | `:316-318` (sessions do not move with slots) |
| `index_find` | IDs in S are filtered out | `:742` |
| minting | S is skipped | `:172` |
| follow-up targeting S | **held** (owned but not served), and released if S later becomes PROVEN; never run | `:1870-1876` |
| resume pass (an object's own outbox) | skipped, so no follow-up is emitted *from* S's objects. The obligations stay durable in the objects and are resumed when S becomes PROVEN | `:1288` (`resume_all`), `resume_slot` |
| `OpCtx::local` | not local | `:1717` |
| standby lease reads | refused | the same predicate |
| `slot_report` / `INFO` | `serving = 0`, plus the reason (section 13) | `:2147` |
| source re-sealed at promotion | held and new follow-ups wait exactly as at any sealed source | existing seal path |

**Liveness cost.** Follow-ups held for an unproven slot wait until repair.
The held queue is in memory and unbounded (MIGRATION.md, Limits), the
same as for a long seal. It is reported as `emits_held`.

## 13. Operator visibility

| Where | What |
| --- | --- |
| `MOVE INFO S` report | new trailing fields `flags` (the slot state's, so `kSlotUnsealed` is visible), `proof=ok|foreign|sealed|staged|missing|stale-map`, and the expected `Ma`; added at the end, so existing parsers (the lab `Probe` reads field 8) are unaffected |
| `STATUS` | new `unproven=N` (slots owned by the map but failing the proof) |
| stderr, once per slot and cause | `snc: shard K: slot S MIGRATION_STATE_INCOMPLETE: placement says arrived by move M at epoch E; local state <phase/move> — not serving` |
| trace | `slot_unproven slot= move= local_phase= local_move=`, and `slot_resealed slot= move=` at the barrier |
| `MOVE ROLE` | `m` while a barrier runs (promoting), then `p` |
| counter | `slots_unproven` in shard stats |
| client | `E_MIGRATING` (decision 3) |

**What this lets an operator see.** A normal migration and missing
evidence can be told apart:

- A normal migration shows `proof=sealed` or `foreign`, and the window
  ends.
- Missing evidence shows `proof=missing` and stays that way.

## 14. Deterministic test plan

**Harness.**

- The lossy promotion follows `test_async_failover.py`: real
  `snc_server` processes with `--async-replica`, the standby held with
  SIGSTOP while the primary acks locally.
- `test_migrate.cpp` runs nodes in process (`Engine eng(c)`), so its groups
  get async replication through `Config`, not a command-line flag. The
  multi-process D7 cases (F7, F13, F17) need a Python harness like
  `test_witness.py`.
- A new `--debug-admin` hook, `DEBUG REPL-PAUSE`, sits next to the
  existing `DEBUG STALL` (`snc_server.cpp:547`). It stops shipping on the
  primary after the current LSN, so tests choose exactly which records the
  standby lacks.
- Driver stop points already exist (`stop_at`).

**What a pass requires.** No test passes on an error status alone. Each
checks, where relevant:

- which groups serve S (the probe on every node);
- object counts in S on each node;
- index results;
- mint targets;
- balances, with money conserved;
- that `emits_held` and the outbox drain or stay held as expected.

| ID | Scenario | Checks | Class |
| --- | --- | --- | --- |
| F1 | **Reproduce D1 on today's code** (characterization, run first): partial staging at B, STAGED local only, commit, lose P_B, promote Q_B | today: Q_B serves S with fewer objects (an expected failure that documents the defect) | safety |
| F2 | **Reproduce D7 path 1 on today's code**: seal local only, export, stage, lose P_A, promote Q_A, then commit | today: Q_A acks writes that B lacks; two groups serving S | safety |
| F3 | D1 with the fix | Q_B: `proof=missing`, S not served, 0 index hits for S, no mint into S, follow-ups to S held, balances unchanged | safety, consistency |
| F4 | D1b: abort M1 after STAGED, move M3 at the same epoch, lose STAGED(M3) at promotion | STAGED(M1) is **not** activated; fails closed | safety |
| F5 | D1c: A to B, B to C, C to B; the replica's prefix ends after ACTIVE of the first arrival | fails closed | safety |
| F5b | D1d: A to B to A; A's replica prefix ends at SEALED of the first move; a driver run calls UNSEAL | unsealed record is older than `Ma`; not served | safety |
| F6 | D7 with the fix: the promoted Q_A reads placement SEALED or STAGED | re-sealed, never serves; commit keeps P_A's image; every write acked before the promotion is present at B | safety, durability |
| F7 | D7 path 3 (two primaries reachable by the driver), including the race where Q_A read PREPARED and is still promoting at the gate | gate refuses while any member answers `m` or serves; no commit; two serving groups never observed | safety |
| F8 | never-moved slot (`Ma = 0`, NONE) | serves exactly as today | availability |
| F9 | completed move, then: a restart of B; a sync healthy failover of B; a restart of A | serves at B with no gap after the first serve; A never serves | availability, safety |
| F10 | aborted move at PREPARED, SEALED, and STAGED (with and without a re-seal) | A serves again after UNSEAL; B never activates STAGED(M) | availability, safety |
| F11 | repeated moves A to B to C to A (10, 20, 30) | only the latest arrival's evidence serves; placement holds one entry for S | safety, boundedness |
| F12 | stale map at B (an older `arrived`) and a stale move ID | refused until the map is refreshed; then serves | safety, liveness |
| F13 | admin `PROMOTE` and witness promotion, both in F3 and F6 | identical outcomes | safety |
| F14 | recovery barrier: SIGSTOP at the first `refresh_slots` and the first `start_serving` of a promoted Q_B; probe in between | S is never `serving = 1` at any sample | safety |
| F15 | promotion with the control plane unreachable | promotion fails (as with no lease block); nothing serves | safety, availability |
| F16 | mixed placement versions: a v1 placement with no `arrived`; slots moved before the upgrade (`Ma = 0`, local ACTIVE) | today's behavior, still served (documented); the backfill sets `arrived` only from ACTIVE records that are not unsealed, and bumps the epoch so running nodes adopt it | compatibility |
| F17 | offline promotion: stop a lagging standby (lossy prefix, stale stored map), restart its directory without `--standby`, for D1, D1c and D7 | barrier runs (`DIR/role = standby`); stored map refused; same outcomes as F3, F5, F6; with the control plane down the start fails | safety |
| F18 | restart of a primary's own directory with the control plane down (`DIR/role = primary`) | starts on its stored map, as today; owned proven slots serve | availability |

Sanitizers: ASan+UBSan and TSan, as for PR #37.

## 15. Compatibility

**Why existing state is not enough** (the work order asks for this before
any format change):

- **Local slot state cannot tell never-moved from lost.** Lossy loss is
  the very loss of those records (D1).
- **The placement's epoch says that some slot moved, not which one.**
  `PlacementMove` is cleared after activation.
- **A "genesis owner" rule fails D1c.** Deriving the original owner from
  `ClusterMap::make` lets a slot that moved away and back serve an
  ancient image after a deep lossy loss.
- **An epoch-only rule fails D1b.** Two moves of S can share an epoch
  (section 1).
- **Node-local `moved_at_`** is volatile. It is rebuilt from map
  transitions the node happened to see, so a fresh or promoted node does
  not know it.

**Therefore the placement needs `arrived[S] = (move_id, epoch)`.**

| Surface | Change |
| --- | --- |
| Placement encoding | `v2`: a new key `arrived=S:move:epoch,...` (sparse). `Placement::decode` rejects unknown keys and versions (`placement.cpp`), so a v1 node cannot read v2. **Rollout:** upgrade every node and the driver/`snc_ctl` first, then let the driver write v2 (a `--placement-v2` switch). Witnesses: none (the body is opaque, `snc_witness.cpp:164`) |
| `ClusterMap` (in memory) | adds `arrived` per slot; `placement_fetch` passes it; no change to the crc rule |
| Durable WAL / checkpoint / `SlotStateV1` | **none**; the re-seal is an ordinary SEALED record |
| Data directory | a new synced `DIR/role` file (`primary`/`standby`), written at start and at promotion, beside `DIR/node`. A missing file is read as `primary`: today's behavior for directories created before the upgrade |
| Public API (`include/snc/snc.h`) | `Engine::Config` gains the startup `arrived` entries (`control::apply` builds `Config` from the resolved placement, `control.h:209-211`) and a `placement_sync` hook; `placement_fetch` and `Engine::update_placement` gain `arrived`. Direct callers to update: `tests/test_resend_refusals.cpp:185, 200` |
| Placement size | `arrived` for all 4,096 slots is about 80-100 KB per encoded line, against a few KiB today. `read_line` allows it, but every witness Paxos write and fsync carries it. Bounded by 4,096 entries |
| Admin protocol | additive: a trailing `proof=` field in reports, `unproven=` in STATUS |
| Client wire | none (`E_MIGRATING`, decision 3) |
| Replication | none |
| Before the upgrade | slots moved under v1 have no `arrived` entry (`Ma = 0`): today's rule until their next move, so D1c/D1d stay open for them. **Backfill** (decision 4): `snc_ctl placement backfill` reads each owner's primary `INFO` (with the new `flags` field). Only where an owner reports ACTIVE(M) **not unsealed**, and the placement shows it as owner, it sets `arrived[S] = (M, st.epoch)`. An unsealed ACTIVE carries the aborted move's ID, not the arrival's, and would brick the slot. The CAS **bumps the epoch** so running nodes adopt it. Nothing is written for slots it cannot confirm |
| Writing v2 | once any process has read a v2 placement, it always writes v2. A driver without `--placement-v2` must not re-encode v1 and silently drop `arrived` |
| Rollback | nodes treat a missing `arrived` as `Ma = 0`. Once v2 placements exist, v1 binaries cannot start: `resolve` decodes the stored `DIR/placement` first and refuses a malformed one (`control.h:152-156`). Rollback needs both `snc_ctl placement downgrade` (rewrite the control plane's copy as v1, dropping `arrived`) **and** rewriting or removing every node's `DIR/placement` |

## 16. Relationship to R08

| | D1/D7 fail-closed migration (this slice) | R08 promotion eligibility (separate slice) |
| --- | --- | --- |
| Guarantee | **Even if a lossy promotion happens, incomplete or stale migration state never serves, and a source that lost its seal never serves a slot once a final export of that seal may exist.** | **No automatic promotion violates the declared durability contract** (e.g. async groups do not auto-promote: CORE_CONTRACTS F-a) |
| Protects | slot-level ownership truth during and after moves | every acknowledged write in the group |
| Does not protect | acked *ordinary* writes lost by the lossy promotion (they are lost; this slice does not hide that); a second primary the driver cannot see (split brain); slots moved before the v2 upgrade and not backfilled | migration state after an operator-forced or otherwise lossy promotion |
| Mechanism | positive proof from placement + local evidence; re-seal at the start barrier; commit gate | witness eligibility rules (E1-E11) |

**Neither replaces the other.**

- With R08 alone, an admin `PROMOTE` (E3) still produces D1 and D7.
- With this slice alone, async promotion still loses acknowledged writes
  (FAILURE_MODEL C7).

## 17. Relationship to S1 (MIGRATION_STAGING.md)

S1 stays as accepted: a volatile FINALIZING chain, staged records as
progress, bounded units and guards. **The assumptions in
MIGRATION_STAGING.md that change:**

1. **Activation.** It now requires `st.move_id == arrived[S].move`, not
   only `epoch >= st.epoch` (MIGRATION_STAGING section 1.4). S1 does not
   activate, so nothing in S1 changes.
2. **Section 2.2, "partial staging is inert".** It becomes unconditional
   (it was conditional on no lossy promotion). Section 6's D1 note
   (C10-C13 under async) is resolved by this slice.
3. **Appendix A.1.** Its sketch (a per-slot `moved` epoch) is replaced by
   `arrived = (move_id, epoch)`. The epoch alone fails D1b. A.1's D7 idea
   (a driver check before commit) becomes the commit gate here, plus the
   start barrier.
4. **The G-map guard** ("`foreign_slot(S)` still true") is unchanged. A
   later repair slice adds one exception: staging into an owned slot
   that is unproven, for the arrival move only. S1 must not assume that
   the final stage only ever targets a foreign slot when it later shares
   code with repair.
5. **`W_window`** is UNDECIDED, as the PR #38 review says. Its earlier
   1 s proposal is withdrawn there.

## 18. Smallest implementation slice

1. **`include/snc/placement.h`, `src/core/placement.cpp`:**
   - `arrived` entries (sparse, keyed by slot) with v2 encoding and
     decoding, and v1 still read;
   - `ClusterMap` carries `arrived_move[]` and `arrived_epoch[]`;
   - `placement_fetch` passes them (`tools/control.h`).
2. **`tools/migrate.h`:**
   - the commit CAS sets `arrived[S]`;
   - the commit gate in `step_staged` (5b);
   - abort instead of looping when the source has no SEALED(M) (5c);
   - always write v2 once v2 was read.
3. **`src/core/shard.{h,cpp}`:**
   - PROVEN, folded into the phase cache as a volatile `UNPROVEN` value
     in `refresh_slots`, `write_slot_state`, `install_indexed` and
     `set_map`;
   - exact `activate_staged`;
   - `seal_slot` refuses an unproven slot;
   - a re-seal step inside `Shard::promote` and before `finish_recovery`'s
     serving pass;
   - an owned-slot pass in `refresh_slots` and `set_map`;
   - `slot_report` gains `proof=`;
   - the `slots_unproven` counter;
   - the `slot_unproven` and `slot_resealed` traces and the log line.
4. **`src/core/engine.cpp`, `include/snc/snc.h`:**
   - `Config` carries the startup `arrived` entries and a `placement_sync`
     hook, like `lease_blocks`; `update_placement` and `placement_fetch`
     carry `arrived`;
   - `Engine::promote` gets the barrier: `PLACEMENT SYNC` and adoption at
     step 0, the re-seal inside `Shard::promote` after `stop_standby`, a
     local-durability wait, then serving;
   - the same barrier in `start` when `DIR/role = standby`.
5. **`tools/snc_server.cpp`, `tools/control.h`, `tools/snc_ctl`:**
   - `DIR/role`;
   - `ROLE m` while a barrier runs;
   - `flags` and `proof=` in `INFO`;
   - `unproven=` in STATUS;
   - `DEBUG REPL-PAUSE` behind `--debug-admin`;
   - `--placement-v2`;
   - `placement backfill` and `placement downgrade`.
6. **Tests:** F1-F18. F1 and F2 run first, on today's code, as
   characterization.
7. **Docs:**
   - MIGRATION.md (activation rule, proof, start barrier);
   - CORE_CONTRACTS: a new migration-serving rule MS-1 to MS-4, with E9
     and E3 notes;
   - FAILURE_MODEL C7, plus a new row for D1 and D7;
   - MIGRATION_STAGING.md cross-references;
   - STATUS, EVIDENCE and the AGENTS handoff.

**Not in the slice:**

- repair, that is, re-staging from the source's sealed copy;
- R08 eligibility;
- a client status code;
- garbage collection of leftovers;
- S1.

**Rough size (INFERRED):** ~500 lines of code and ~700 of tests.

## 19. Conflicts with CORE_CONTRACTS, FAILURE_MODEL and MIGRATION.md

| Document / rule | Relation | Note |
| --- | --- | --- |
| MIGRATION.md state machine: "STAGED becomes ACTIVE once the map gives S to B at epoch ≥ E+1" | **REFINE** | plus `move_id == arrived[S].move` (D1b) |
| MIGRATION.md proof (safety: never two writing groups) | **REFINE**: its HA-durability assumption made explicit | the proof assumes acked SEALED/STAGED survive failover; true only for sync-healthy groups. This slice makes safety hold without that assumption for one-primary histories |
| MIGRATION.md Limits: "An aborted move leaves its partial copy at the destination, unserved; a later move to it replaces it" | KEEP, now enforced | exact activation makes "unserved" unconditional |
| `tools/control.h` comment "seals, not the map alone, decide what a node serves" | **REFINE** | seals *and arrival proofs* |
| CORE_CONTRACTS E3 (admin PROMOTE has no term guard) | unchanged; consequence narrowed | admin PROMOTE now passes through the barrier; its general unsafety stays R08/E3 work |
| CORE_CONTRACTS E9 (promotion during a move) | **REFINE** | promotion reads placement and re-seals; to be tested (F6, F13) |
| CORE_CONTRACTS E11 (lease block before serving) | KEEP; extended | the placement read joins the same step and fails the same way |
| CORE_CONTRACTS section 12 (recovery barriers) | KEEP | PROVEN computed before the serving pass |
| Invariant 11 (failover is not migration) | KEEP, with a note | promotion now *reads* placement state; it does not change placement or the epoch |
| Invariant 12 (epoch and term independent) | KEEP | the proof uses move ID and epoch; no term enters it |
| Invariant 14 (hot path reads cached control state) | KEEP | PROVEN is cached; one linearizable read per promotion, none per op |
| Invariant 17 (node-wide map adoption) | KEEP | PROVEN is recomputed in pass 1 (`set_map`) before held work is released |
| FAILURE_MODEL C7 (stale replica promoted) | **REFINE** | adds the migration consequences (D1, D1b, D1c, D1d, D7) and their status, and names the offline promotion path (`snc_server.cpp:27-29`) |
| `tools/snc_server.cpp` header (offline promotion by restarting without `--standby`) | **REFINE** | that start now runs the barrier and needs the control plane |
| FAILURE_MODEL "cut-off primary" gap | unchanged | a second primary the driver cannot reach is not closed here (section 5) |
| MIGRATION_STAGING.md sections 1.4, 2.2, 6, A.1 | **REFINE** | section 17 above |

## 20. As built (implementation slice, after the PR #39 review)

**Characterized first (review condition 12).** F1 (D1) and F2 (D7), then
D1b, D1c and D1d, were run against the code before the fix (main at
4c92093 plus test instrumentation only) and reproduced exactly as
predicted; evidence in
`bench/results/failclosed-20261104/characterization.txt`
(`SNC_CHARACTERIZE=1 tests/test_fail_closed.py`):

| | Before (MEASURED) | After (MEASURED) |
| --- | --- | --- |
| F1 D1 | promoted Q_B serves S, phase NONE, 31 of 41 accounts; 10 `E_NOT_FOUND`; an acked deposit missing; a create in S succeeds | `proof=missing`, every op `E_MIGRATING`, create refused, a credit into S held; never sampled serving |
| F2 D7 | promoted Q_A serves S and acks a write; after the commit two groups serve; the write is lost at B | Q_A re-sealed at the barrier (phase SEALED, new term), the write `E_MIGRATING`; one serving group; DONE with every write acked before the promotion at B |
| F4 D1b | STAGED(M1) of an aborted move activated although M3 committed | not activated (`proof=missing`) |
| F5 D1c | ACTIVE(M3) of an earlier era served at epoch 4 | not served |
| F5b D1d | SEALED(M5) reopened by UNSEAL and served | not served |

**Where it is in the code.**

- Placement v2 (`include/snc/placement.h`, `src/core/placement.cpp`):
  `v2 ... arrived=SLOT:MOVEHEX:EPOCH,...`, sparse, ascending, every epoch
  at most the placement's; `encode` refuses a v1 placement that carries
  arrivals (no silent downgrade); decode rejects unordered, repeated,
  zero, non-canonical or out-of-range entries. The driver writes the
  arrival in the commit CAS when the placement is already v2 or
  `--placement-v2` was given, and every rewrite starts from the placement
  it read, so v2 stays v2. `snc_ctl --placement-v2 init|move|placement
  backfill` is the only way v2 is first written.
- The proof (`src/core/shard.cpp` `proven`, `cache_value`, `set_cache`):
  folded into the per-slot serving cache as a volatile fourth value
  (`kCacheUnproven`), recomputed by `write_slot_state`, `install_indexed`,
  `refresh_slots` (now every owned slot) and `set_map`, which recomputes
  before `activate_staged` and `release_held`. Everything that asks
  `serving()` fails closed: client ops, creates and mints, deletes, index
  lookups, follow-ups (held), resumes, exports and seals (`E_MIGRATING`).
  `INFO` adds `flags=`, `proof=` (`ok`, `foreign`, `sealed`, `staged`,
  `missing`, `stale-map`) and `arrived=`; STATUS adds `unproven=N`; the
  log line `MIGRATION_STATE_INCOMPLETE` and the `slot_unproven` trace.
- Exact activation (`activate_staged`): the arrival's move must equal the
  STAGED record's (or no arrival is on record). A STAGED record older than
  the arrival reports `proof=missing`: it can never activate.
- DIR/role (`src/core/engine.cpp` `read_role`, `write_role`): exactly
  `primary\n` or `standby\n`, written to a temporary file, fsynced,
  renamed, directory fsynced; anything else reads as malformed. A standby
  writes `standby` before the superblock, seed or any replica record. A
  superblock flag (`kSbRoleTracked`, in the formerly reserved field, which
  older binaries ignore) marks directories that have had a role file. A
  directory with neither (from before role files) does not start as a
  primary unless adopted once (`--adopt-role primary`, checked, through the
  barrier): [LEGACY_ROLE.md](LEGACY_ROLE.md), after the review of this
  slice.
- The start barrier: in place (`Engine::promote`) the placement sync comes
  first (a failure changes nothing: "promote refused"), then the lease
  block, adoption, settling the replica prefix, the re-seal inside each
  shard's promotion before `start_serving`, then a wait for the re-seal's
  local durability, then `role primary`. Offline (`Engine::start` of a
  directory whose role is standby, malformed, or missing after role files
  exist): the placement sync before any shard starts (the stored map is
  replaced), the re-seal before `finish_recovery`'s serving pass, then the
  same durability wait, a second sync (the move may have advanced), and
  `role primary`. With no control plane the start fails. `ROLE m` is
  answered while an in-place promotion runs.
- The barrier seals at PREPARED too (not only SEALED or STAGED): the
  driver seals next anyway (SEAL is idempotent for a move), and an offline
  start, which the gate cannot see, must not serve a slot a move may be
  about to take. An abort unseals it as for any seal.
- The re-seal is `reseal_slot`: an ordinary SEALED record of the move,
  written under the new term; a slot already SEALED for that move keeps
  its record; an unproven slot is left alone (nothing is written that
  could later look like proof).
- The driver (`tools/migrate.h`): the commit gate (`ROLE m` anywhere in
  the source group waits; no answering primary waits; a primary serving S
  is retried twice more and then the move is aborted, "ABORTED commit
  gate: ..."); a source without SEALED(M) at the stage step aborts the
  move; a
  destination that already holds a durable STAGED(M) skips the re-export;
  a destination that is unproven at activation leaves the driver
  `STUCK ... proof=missing` and clears the move record (the arrival stays
  on record and the source keeps SEALED(M), which a repair needs; other
  moves are not blocked). A final stage refused `E_VERSION_CONFLICT`
  three times in one driver run (a race settles on a fresh HAVE; a source whose
  sealed image is older than the destination's copy never does) aborts
  the move.
- Backfill (`snc_ctl --placement-v2 placement backfill`,
  `Driver::backfill`): reads the placement linearizably, refuses while a
  move is in progress or any group has no answering primary, and reads
  every group primary's slot states (`MOVE STATES`). A slot gets an
  arrival only when its owner holds ACTIVE(M), not unsealed, durable, at
  an epoch the placement has reached, and no other group holds a later
  move of it or anything for M but its source's seal. One CAS, epoch + 1,
  idempotent; everything else is listed as `UNRESOLVED slot:reason`.
- Test hooks, all behind `--debug-admin`: `DEBUG REPL-PAUSE 1|0` (every
  replication sender in the process stops shipping, without timing out)
  and `--debug-barrier hold:MS|crash` (at the end of the barrier, before
  `role primary`).

**Differences from the design.**

- `placement downgrade` was not built (review item 15). Rolling back after
  v2 is the manual procedure below.
- `ROLE m` exists only for the in-place promotion: an offline start has no
  admin port until its barrier is done, so the gate does not see it
  (residual 2). In exchange the barrier also seals at PREPARED, which
  removes the design's path-2 exception (section 5, and the SEALED row of
  section 7): no barrier serves a slot the placement shows moving away.
- The aborts of 5a/5c (no seal at the source) plus one more: a final
  stage refused with a version conflict three times in one driver run.
- A durable format addition beyond `DIR/role`: the superblock's formerly
  reserved field is now `flags`, with `kSbRoleTracked` (older binaries
  ignore it), so that a missing role file after role files exist is not
  taken for a legacy primary's (section 15 said `DIR/role` only).
- The driver clears a COMMITTED move whose destination is unproven, with
  STUCK, instead of leaving it (MIGRATION.md's state machine gains that
  outcome).
- An in-place promotion starts serving the other slots before the re-seal
  is durable (`Shard::promote` runs `start_serving`, then
  `finish_barrier` waits); S itself is SEALED in memory before anything
  serves, and a crash before durability runs the barrier again. Precisely
  (review of PR #40, item 8): **the node may serve independently proven
  unaffected slots while the migration barrier finishes; the affected slot
  remains non-serving, and the directory is not declared primary until
  barrier durability completes.** The whole node does not wait to serve;
  section 5a's "only then serve" holds for S.
- F14 samples `MOVE INFO` every 5-10 ms across in-place and witness
  promotions instead of stopping the process at `refresh_slots` and
  `start_serving`.
- A resume of a slot (after an UNSEAL) now runs only if the slot serves:
  an UNSEAL that leaves a slot unproven (D1d) re-emits nothing from its
  stale objects (found by the independent review).

**QA against MIGRATION_FAIL_CLOSED.md, CORE_CONTRACTS, FAILURE_MODEL and
MIGRATION.md** (a second separate reader): every section 4-9, 12, 13
and 15 requirement implemented, nothing unauthorized built; two plan
items lacked tests (F10 with a re-seal, F7's PREPARED race: added), two
small gaps (leading zeros accepted in `arrived=`; no log line for a stale
STAGED: fixed), and doc drift in this section, CORE_CONTRACTS MS-1/MS-3
and E3, FAILURE_MODEL C7a, MIGRATION.md and MIGRATION_STAGING.md
(fixed).

**Independent review** (a separate reviewer reading the diff against the
ten review conditions; nothing run): conditions 1, 2, 5-8 and 10 met; 3
and 9 met except one blocking finding (the resume after an UNSEAL, above),
fixed with a test that fails without the fix (`test_proof`); 4 met for
in-place promotion, the offline path's missing `ROLE m` handled by sealing
at PREPARED (`prepared` scenario). Also fixed: a STUCK move blocking all
other moves (`stuck` scenario), the driver aborting on one version
conflict, counters not reset between moves, a repeated `arrived=` key
accepted, a missing parent directory of a new standby. No backfill false
positive was found.

**Tests.** `tests/test_fail_closed.py` (real processes, three witnesses,
two or three groups; `ctest` runs every scenario that needs no older
build): F1-F11, F13 (`f13:` prefix, witness promotion), F15, F17
(`f17:` prefix: the standby's directory restarted without `--standby`,
refused while the control plane is down), F18, `role` (review 13A: crash
after the standby marker, crash with the barrier held open, crash before
the primary marker, truncated / malformed / empty / missing role file),
`backfill` (13B idempotence and 13F refusals: a move in progress, a group
without a primary, owner unsealed or not active), `stuck`, `prepared`
(an offline start at PREPARED seals), `f10_reseal` (a re-seal at
promotion cleared by the abort's UNSEAL), `f7_prepared` (in-place
promotion at PREPARED, held at ROLE m, while the driver runs) and
`size`. With
`--old` (a build of main before this slice): `compat` (a v1 cluster of
old binaries, upgraded in place; since [LEGACY_ROLE.md](LEGACY_ROLE.md)
every pre-upgrade directory is refused until started with `--standby` or
adopted with `--adopt-role primary`; the backfill through the old,
unchanged witnesses; an old `snc_ctl` can neither read v2 nor write
anything) and `f7b` (a reachable source primary that serves S: the gate
aborts the move). `tests/test_proof.cpp`: 13C (no window after map
adoption: the first op refused, no index hit, no create, a held credit),
13D (STAGED(0x20) never activates for arrival 0x30; activates for 0x20),
13E / F12 (a restart on a map whose arrival is older than the local
ACTIVE: `stale-map`, unserved until the current placement). F14 is the
probes inside F1, F3, F6, F7, f7_prepared and F13 (MOVE INFO sampled
every 5-10 ms across in-place and witness promotions: never serving; an
offline start has no admin port to sample until its barrier is done). `tests/test_placement.cpp`:
13B text rules.

**Runs (2026-11-04, this VM: 4 vCPU, loopback; fresh build directories,
after every review and QA fix).** RelWithDebInfo `ctest`: 20/20 passed,
including `test_migrate` (the existing migration crash matrix) and
`test_crash`; `test_fail_closed` ran its 24 scenarios in 140 s. ASan+UBSan
(Debug, `-fno-sanitize-recover=undefined`): 20/20. TSan (Debug,
`halt_on_error=1`): 20/20. The Python tests ran the sanitized server and
witness binaries too; a sanitizer report aborts the process, and none
did. `compat` and `f7b` against a build of main before this slice: ok.

**Placement size (MEASURED, loopback, 4 vCPU; review item 14).**

| | v1 (2 groups) | v2, 256 arrivals | v2, all 4,096 arrivals |
| --- | --- | --- | --- |
| encoded | 266 B | 6.3 KB | 101.6 KB (small epochs); 146.6 KB with 16-digit move IDs and 13-digit epochs |
| CAS (3 witnesses, Paxos + fsync) | 8.4 ms | 8.7 ms | 14.1 ms |
| SYNC | 3.4 ms | 3.4 ms | 6.1 ms |
| witness state on disk | | | 101.7 KB per witness |
| encode / decode / to ClusterMap | | | 0.48 ms / 1.4 ms / 7 µs |
| memory | | | 64 KiB per Placement and per ClusterMap |

Startup load is one decode of the stored placement (1.4 ms at worst).
Nothing was optimized.

**Rollback after v2 (manual, unsafe; review item 15).** There is no
command. Before any v2 placement exists, rolling back is replacing the
binaries. After: (0) check that no move is in progress and that every
node reports `STATUS unproven=0` (an old binary would serve an unproven
slot's partial copy: D1); (1) stop every driver; (2) accept that all of
this slice's protection is gone: D1, D1b, D1c, D1d and D7 reopen, and
the commit gate and start barrier do not exist in the old binaries; (3)
with every node stopped, read the placement and its version with
`PLACEMENT SYNC` on a witness and write a v1 body (the same text with
`v2` changed to `v1` and the `arrived=` key removed, epoch + 1) with a
raw `PLACEMENT CAS VERSION BODY` (an old `snc_ctl` cannot decode v2);
(4) delete or rewrite every node's `DIR/placement` (an old binary refuses
to start on a v2 file); (5) start the old binaries.

**Residual risks (not closed here).**

1. A second source primary the driver cannot reach (a cut-off async
   primary) can still serve S after the commit: R08 / "cut-off primary".
2. An offline start of a former standby answers no `ROLE m` (its admin
   port opens after the barrier), so the gate cannot see it. It seals at
   PREPARED, SEALED or STAGED before anything serves and syncs again once
   that is durable, so it never serves a slot the placement shows moving
   away from its group. A move that is *prepared* after its last sync
   needs this group's primary to seal: if that is this node, the driver
   waits for its admin port; if another member is also acting as primary,
   that is residual 1. An in-place promotion that read a move which is
   then aborted keeps its re-seal until a driver run for the slot unseals
   it (liveness only).
3. Slots moved before v2 are protected only once backfilled; the backfill
   leaves what it cannot prove unresolved.
4. An unproven slot stays unavailable until repair (not built).
5. Acknowledged writes lost by an async promotion stay lost (R08).
6. ~~Upgrade order: a pre-role-file standby restarted without
   `--standby` is taken for a legacy primary's.~~ **Closed** by
   [LEGACY_ROLE.md](LEGACY_ROLE.md) (after the review of PR #40): such a
   directory is refused unless started with `--standby` or adopted with
   `--adopt-role primary` (checked against the group's members, through
   the barrier, crash-safe). Still open: a rollback to an older binary
   and back can leave a stale `primary` file on a directory that served
   as a standby meanwhile (older binaries do not write it).

