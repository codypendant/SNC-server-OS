# Slot moves: a safety protocol

A slot moves from one replication group (the **source**, A) to another (the
**destination**, B) while both keep serving everything else, and while
any party may crash (docs/CONTROL_PLANE.md section 9 and step 3 of
section 12). This document defines the protocol, proves the two
properties it must keep, and says how the tests check them.

**Safety.** At no instant do two replication groups both accept writes for
one slot.

**Recovery.** After any recoverable interruption the move either completes
or is aborted, deterministically, leaving one authoritative owner and every
acknowledged write.

```
snc_ctl --control H0:9000,H1:9000,H2:9000 move 1202 1      # move slot 1202 to group 1
snc_ctl --control H0:9000,H1:9000,H2:9000 move --abort 1202
```

Either command, run again after any failure (of itself, a node, or a
control-plane minority), carries on from where the move is.

## The pieces

**Placement** (the witnesses' `placement` machine, docs/CONTROL_PLANE.md
section 10) holds the slot map, its epoch, and at most one **move record**
`{id, slot, from, to, epoch, state}` (`PlacementMove`,
`include/snc/placement.h`). Every change is a compare-and-set on the
machine's version (`PLACEMENT CAS V body`: chosen only if the latest version
is still V), so concurrent drivers serialise and a stale one can never
advance a move it has not seen. Decisions that must not act on a stale view
read linearizably (`PLACEMENT SYNC`: a Paxos phase 1 on the next version,
finishing any value already accepted, `tools/paxos.h` `read_linear`).

- `move.id` is the machine version that records the PREPARED move, so it
  grows with every move.
- `move.epoch` is the placement epoch plus 1 when the move is prepared,
  the epoch it commits at.
- The epoch changes in exactly one transition: the commit, which sets
  `slots[S] = to` and `epoch = move.epoch` in the same chosen value.

**Slot state** (`System.SlotState`, type 2, `src/core/slot_state.h`) is the
group's own record of a slot that took part in a move: one small object
per slot, on the shard that owns the slot, logged, replicated to the sync
standby, checkpointed and seeded like any object. So a seal, a stage or an
activation survives a crash or a failover exactly as an acknowledged write
does. Its phase is one of:

| Phase | Meaning |
| --- | --- |
| NONE (no record) | serving iff the map gives this group the slot |
| ACTIVE | as NONE (activated here by a move, or unsealed) |
| SEALED `{move, to, epoch, count, digest}` | moving away: never applies another write to the slot, whatever the map says |
| STAGED `{move, from, epoch, count, digest}` | moving here: an exact copy of the sealed source, not yet authoritative |

**The serving rule.** A node of group G serves slot S (runs ops on its
objects, mints IDs in it, runs follow-ups to it, resumes its objects) iff
it is G's primary, not fenced, its map gives S to G, and G's phase for S
is NONE or ACTIVE (`Shard::serving`). The check runs on the shard that
owns S, the same thread that writes the seal, so an op either runs before
the seal's record (and is in the log ahead of it) or is refused. Refusals:
`E_MOVED {epoch, group}` when the map gives S to another group,
`E_MIGRATING {epoch, group}` when the map gives it to this group but the
phase does not (sealed: `group` is the destination).

## The state machine

The driver (`migrate::Driver`, `tools/migrate.h`) keeps no state. Each pass
reads the placement linearizably and does the step its state asks for; each
step is idempotent. The node side is `migrate::NodeHandler` (requests
`MOVE ...` on a node's admin port).

| Placement | Step (driver) | Durable result | Then |
| --- | --- | --- | --- |
| no move, `slots[S] = A` | CAS: add `{id, S, A, B, E+1, PREPARED}` | the record | PREPARED |
| PREPARED | **copy ahead**: B sends what it holds of S (`id, version, crc`); A exports its *durable* objects that differ; B stages them (chunks, not final) | objects at B, not served | |
| PREPARED | **seal** at A (only if the placement shows this move PREPARED) | SEALED at A, durable on its standby | CAS: SEALED |
| SEALED | **final copy**: B's list again; A exports all of S (only once its seal for this move is durable), payloads for what differs; B makes its copy equal (adds, replaces, drops the rest), checks count and digest, writes STAGED | STAGED at B, durable on its standby | CAS: STAGED |
| STAGED | CAS: `slots[S] = B`, `epoch = E+1`, state COMMITTED | the new map | COMMITTED |
| COMMITTED | **activate** at B: refresh its map; STAGED becomes ACTIVE once the map gives S to B at epoch ≥ E+1 | ACTIVE at B | push the map to every node; CAS: remove the record |

**Abort** is a CAS removing the record while its state is not COMMITTED,
then an **unseal** at A. A unseals only after a linearizable read shows
`slots[S] = A` and no move with its seal's ID (`NodeHandler::unseal`): the
removed move can then never commit, as a record is only re-created by a new
PREPARE, which gets a new ID. After COMMITTED a move can only be finished
(`move --abort` answers `NO committed`). Any driver run that finds no move
of S also asks its owner to unseal (`Driver::settle_owner`), which clears a
seal left by a driver that died between the abort CAS and the unseal.

Late requests are refused. A node seals or stages only for the move the
placement shows, at the step that asks for it (`NodeHandler::current`),
and a group refuses a seal or a stage for a move older than the one it
last recorded, or for a move whose seal it undid (`Shard::seal_slot`,
`stage_slot`). So a driver that stalls and is overtaken (its move aborted,
or finished by another driver) changes nothing when it wakes.

### Copying

- The copy ahead of the seal takes **durable state only**: a version that a
  failover of A could lose must never be staged.
- `crc` covers type, schema and payload, so `(version, crc)` names an
  object's state exactly; the digest is an order-independent sum of a
  hash of `(id, version, crc)` (`slot_object_digest`).
- The final copy happens once A's seal is durable, so nothing on S changes
  at A afterwards. B's final stage is all or nothing for the STAGED record:
  if its copy does not match the count and digest A recorded, nothing is
  recorded and the driver tries again. STAGED is written after every staged
  record in the shard's stream, so it is durable only once they are.
- B refuses to stage into a slot its own map gives to its group (there it
  can only be a source).

## Proof

Write `serves(G, S, t)` for "some node of G serves S at instant t" (the
rule above). Within one group, the existing HA protocol allows at most one
node to acknowledge writes per term, fences the others, and a promoted
standby holds every durable record (docs/REPLICATION.md, invariants 3 and
16). So a group's slot state behaves as one durable register: what a step
reports as done is in it after any failover, and anything not yet durable
was never acknowledged. Maps on a node only move forward (a node adopts
only a newer epoch; its stored copy is written only from a newer fetched
one), and every map is a value the placement machine chose.

**Lemma 1 (epochs).** The placement epoch changes only at a commit. The
only transition that changes it is STAGED → COMMITTED, and the CAS makes
each transition happen at most once per move.

**Lemma 2 (a seal is permanent unless its move is dead).** Once A's seal
for move m is durable, A applies no write to S unless it unseals, and A
unseals only when a linearizable read shows no record of m and
`slots[S] = A`. A removed record never returns (new PREPAREs get new IDs),
so m never commits after that read. Hence: if m commits, A is never
unsealed for m.

*Every write A ever acknowledged on S is in its log before the seal record*
(same shard thread; acknowledgements wait for durability, and the log is a
prefix after any crash).

**Lemma 3 (activation implies commit).** If B holds STAGED(m) and some node
of B has a map with epoch ≥ m.epoch that gives S to B, then m committed.
A map with epoch ≥ m.epoch was chosen after m was prepared. From m's
PREPARE until its record is removed no other move of S exists, so if m was
aborted, the map's `slots[S] = B` comes from a later move m' committed to
B. Committing m' needs B's final stage for m' (state STAGED in placement),
after which B refuses any stage for the older m (IDs). So B could not hold
STAGED(m) now. Hence m committed.

**Theorem (safety).** There is no instant at which A and B both serve S.

Take the last move m of S that was committed before t, say from X to Y
(if none, the map has never given S to anyone but its first owner, and no
group other than it can serve: serving needs the map). Every other group
Z ≠ Y cannot serve S at t: its map at t either predates m's commit (then
`slots[S]` names m's source X or an earlier owner; if it names Z = X, X is
sealed for m by Lemma 2 and the seal precedes the commit, so X does not
serve; if it names an earlier owner, that owner was sealed by the same
argument for the move that took S away from it), or it is at least m's
epoch, which gives S to Y, not Z. Y serves only after activating (STAGED →
ACTIVE needs Lemma 3's condition) or, for a group that never staged S,
never. So at most Y serves. A sealed group never serves, whatever map it
starts from, which is why a node may start from its **stored map** when the
control plane is down: a stale map can make a node refuse a slot it owns,
never accept one it does not (this settles docs/CONTROL_PLANE.md section 9's
provisional point).

**Theorem (no acknowledged write lost, IDs unchanged, no duplicates).** By
Lemma 2 everything A acknowledged on S precedes its seal; the final export
runs on A's primary after the seal is durable, so it contains all of it;
B's copy equals that set by count and digest; B serves S only from it.
Objects keep their IDs (copied as they are), and at every instant exactly
one group serves them. Writes acknowledged by B after activation are B's
ordinary durable writes.

**Recovery (each interruption).**

| Interrupted after | What is durable | Next driver run |
| --- | --- | --- |
| prepared | the record | copies, seals, ... (or abort: remove the record) |
| copying, copied | the record; partial copy at B (not served) | copies what differs again, seals, ... (or abort) |
| sealed | SEALED at A; SEALED in placement | final copy, STAGED, commit, ... (or abort: remove, unseal A) |
| staged | STAGED at B; STAGED in placement | commit, ... (or abort: remove, unseal A; B's copy stays unserved) |
| committed | the new map | activate B, clear the record (abort refused) |
| activated | ACTIVE at B | push the map, clear the record |

A failed primary of either group: its standby is promoted with every step
the driver saw completed (each was reported only once durable, and the
standby is sync). A failed standby: its primary continues alone (gated by
the witnesses in a deployment). A control-plane minority: the placement
and lease machines continue. A control-plane majority: no step advances;
slots keep being served as they were, except a sealed slot, which stays
unavailable (never unsafe) until the control plane is back. A restarted
node starts from its stored map and serves only what the rule above allows;
its map catches up from the control plane later.

## Follow-ups across a move

Transfers between objects settle with follow-ups (docs/TRANSFERS.md), and
a follow-up may reach a node before or after either side adopts the new
map. The transfer protocol relies on one property that a move can break:
the copies of a follow-up between two objects arrive in order, so a credit
copy is never applied after the exchange it belongs to has finished and
been forgotten (the inbox entry that de-duplicates it is gone). The crash
tests found three ways a move broke it, each now closed:

1. A copy sent by the source's old owner overtaking or trailing one its new
   owner sends.
2. A copy that waited for durability because its target was remote, while
   a later copy, emitted after the target became local, did not wait.
3. Copies held for a newer map released after newer traffic had run.

The rules (`src/core/shard.cpp`, `Shard::stale`, `on_link_frame`,
`run_emit`, `set_map`):

- **Every follow-up carries the placement epoch it was emitted at** (node
  link frames carry it whole; within a node it travels in the ring message).
- **Newer than this shard's map: held** until this shard adopts it. So
  nothing is routed or run by a map older than the one it was emitted
  under, and the node refreshes its map at once (the hint wakes its
  placement watcher; otherwise it polls every `placement_poll_ms`).
- **Stale: dropped.** A follow-up emitted before its source's or its
  target's slot last moved (each shard records, per slot, the epoch it
  last changed group). Nothing is lost: when a node adopts a map it re-runs
  every resume hook (`Shard::map_changed`), re-sending all it has
  unsettled, stamped with the new epoch.
- **Adoption is two passes over the node's shards**: every shard first
  routes by the new map and releases what it held (ahead of anything that
  arrives later), then every shard re-sends what is unsettled, so nothing
  re-sent is routed by a shard that still has the old map.
- A follow-up whose target is in a slot this group owns but does not serve
  (sealed or staged) is held, and released when the slot is activated or
  unsealed, or the map changes.

## Clients

- `E_MOVED` with a newer epoch: `net::ClusterClient::on_moved` fetches the
  map from the members (`refresh`) and the request is sent again to the new
  owner. `E_MIGRATING`: wait and send again (and refresh now and then).
- **A resent sequenced request gets its session's answer before any
  refusal.** A node checks the session before refusing a request for a slot
  it no longer serves (`Shard::execute`, `create`). Otherwise a write
  applied just before a failover, resent to the promoted standby whose map
  had moved on, would be answered `E_MOVED`, and the client would apply it
  again at the new owner (found by the crash tests).

## What is checked

`tests/test_migrate.cpp` (ctest `test_migrate`), 67 scenarios, three in
parallel. Each has three witnesses holding placement and leases, two
groups of a primary and a sync standby started by NodeID from the control
plane, and slot S holding 25 of the 57 accounts. A session client
(`ClusterClient`) runs transfers between all accounts, half from S, for
the whole scenario. A first driver runs the move to slot S's destination
and dies after a step; one party fails; a second driver finishes or
aborts.

- Stop points: prepared (before the copy), copying (half staged), copied
  (before the seal), sealed and staged (after the seal, before the
  commit), committed (before activation), activated.
- Failures at each point: none (driver only); source or destination
  primary SIGKILLed and its standby promoted; source or destination standby
  SIGKILLed; one witness SIGKILLed; source or destination primary SIGKILLed
  and restarted from its stored map with all three witnesses down (then the
  witnesses restart).
- Also: abort at every point (refused after the commit, which is then
  finished), aborts after a source or destination failover, a move with no
  pushed map and 60 s polls (nodes must refresh because a node link shows
  them a newer epoch), and a move back to the first group (epoch 3).

Checked in each:

- **Never two serving owners**: a probe asks every node for S's state
  throughout, group X then Y then X again; any node of X serving both
  times while one of Y serves in between would prove an overlap.
- **Every acknowledged transfer is in the final balances**, exactly; the
  only freedom is the few the client got `E_REPLAYED_OLD` for after a
  failover (decided, outcome not kept), each solved for as applied or not.
  Money is conserved.
- **ObjectIDs unchanged and no duplicates**: the accounts served at the end
  are exactly those created, each by one primary.
- **Epochs**: 1 until the commit, 2 after (3 after the move back); no node
  ahead of the placement; the slot map otherwise unchanged; the move record
  gone.
- **Everything settles**: no outbox or inbox entry left.
- **Resolution**: the second driver answers DONE or ABORTED as expected;
  late seal and stage requests for the first move are refused.
- The slot's state on each group and its standby: SEALED at the old owner,
  ACTIVE at the new, standbys equal to their primaries; at a restart, the
  source serves from its stored map only before its seal, the destination
  only once activated with a map at the commit's epoch.

Result: all 67 pass, run 8 times in a row (536 runs), and under ASan.

## Next (accepted by Codex, 2026-10-13; not built)

### Chunked migration stream

The protocol stays as it is; only the transport of a copy changes. Today
the driver reads a slot from the source and writes it to the destination
as one admin line each way. The replacement is a stream per copy:

```text
BEGIN(move, slot, kind = ahead | final)
CHUNK(seq, objects...)*          bounded size, resumable from the last seq
VERIFY(count, digest)            the values the source recorded
STAGE                            unchanged: all or nothing, as above
```

- The destination keeps chunks in a scratch area keyed by `(move, kind)`;
  nothing in it is served or counted until `STAGE`, and a new `BEGIN` for
  another move discards it, so a stalled driver's chunks are as inert as
  its late stage request is now.
- `VERIFY` is the same count and digest check; a mismatch records nothing.
- The stream is an interface over a byte transport (TCP today), not part
  of the admin line protocol, so it can later run over the
  networking-OS/DPDK transport (invariant 10). Source to destination
  directly, with the driver only starting it, keeps the copy off the
  driver's host.

### Reconciling an abandoned seal

Today a seal left by a driver that died mid-abort stays until a driver runs
again for the slot (see "Limits"): unavailable, never unsafe. The future
rule lets the source group clear it by itself, under exactly the condition
the driver's unseal already uses, and nothing weaker:

A sealed source may unseal slot S, sealed for move M, only after a
linearizable placement read (a majority read, as `PLACEMENT SYNC`) shows
both that no move record for M exists and that the committed map still
gives S to this group.

- Both halves are needed. "M is gone" alone also holds after M committed,
  when S belongs to the destination; unsealing then would make two
  serving groups. "S is still ours" alone also holds while M is PREPARED
  or SEALED.
- Move IDs never repeat, so a later move of S cannot be mistaken for M.
- The unseal is logged and replicated like any `SlotState` change; a
  failover between the read and the unseal repeats the read.
- If the control plane cannot be read, the seal stays. Availability never
  overrides the proof.

## Limits

- One move at a time across the cluster (the placement holds one record).
- The source keeps its sealed copies of the slot's objects (unserved,
  inert); a later move back reuses them as the destination's "have" list.
  Nothing collects them yet.
- An aborted move leaves its partial copy at the destination, unserved; a
  later move to it replaces it.
- A seal sent just before its move was aborted can stay; the slot is then
  unavailable, never unsafe, until a driver runs again for it (`move` or
  `move --abort`). Nothing notices it by itself (see "Reconciling an
  abandoned seal").
- Follow-ups held for a newer map are in memory: a node that can never
  refresh keeps holding them (their sources re-send them later anyway).
- Reads are not forwarded to the new owner; clients get `E_MOVED` and
  refresh.
- The driver is an operator tool: `PLACEMENT CAS` accepts any well-formed
  placement from anyone who can reach the witnesses.
- Copy requests carry whole slots in one admin line; fine for a test-sized
  slot, not for a production one (see "Chunked migration stream").
