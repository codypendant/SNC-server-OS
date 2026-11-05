# Sync readiness: when a standby may be promoted (B2)

**Status: IMPLEMENTED 2026-11-05** (work order
`handoff/PR47_PR48_Blocker_Fix_Handoff.txt`, B2). This fixes BLOCKER B2 of
[BASELINE_READINESS.md](BASELINE_READINESS.md) section 3. It covers Profile
B (sync replication with witness HA). It changes no state format: the
witness state file and its Paxos machine are as before. Protocol
additions: a seed status 3 ("retry") and a witness refusal "NO not
learned yet" (section 2). It does not implement any async (R08) policy;
it does touch async groups in one way, which is more conservative
(section 5).

## 1. The invariant

> A standby is promotion-eligible only after its current-term primary has
> positively proven that this standby holds every write the primary has
> acknowledged, and nothing since has made that untrue.

Each of these alone is **not** proof: a connected standby, a started
standby, a standby of the same term, or a standby that answers HEALTH or
STATUS. If the only standby is not proven, nobody is promoted: the group
is unavailable, which is correct. Serving stale state is not.

## 2. The readiness fact

The witnesses' state for a group already holds
`(version, uuid, term, degraded, primary)`. The `degraded` bit is kept,
and its meaning becomes **NOT READY**. `degraded=false` means READY: the
primary of `term` has proven that its standby is in sync.

| Event (Paxos-chosen) | Before | Now |
| --- | --- | --- |
| First learning of a primary (uuid empty) | READY | **NOT READY** |
| Promotion to term T+1 | copied the old term's bit | **NOT READY** |
| `DEGRADE uuid T` from the primary of T | NOT READY | NOT READY (unchanged) |
| `SYNCED uuid T` from the primary of T | READY | READY (unchanged; refused unless T is the current term) |

The promotion check is unchanged: the witnesses promote only from READY,
and only a standby with the same uuid and term. A witness answers
DEGRADE and SYNCED only once it has learned the pair (`NO not learned
yet` before that; the primary asks again), so learning, which records
which node is primary, always comes first.

**A stale SYNCED cannot authorize a promotion.** SYNCED names its term.
The witness accepts it only if that term is the current one (the existing
check, which answers `NO term T was replaced by U`). A promotion moves the
state to T+1 and NOT READY in the same chosen value. A SYNCED for T that
arrives later is therefore refused. One for T+1 can only come from the
primary of T+1.

## 3. SYNCED: what the primary must have seen

The primary of term T sends `SYNCED uuid T [group]` only when **every
shard** is ready. A shard is ready when all of these hold, read on its
own thread:

1. It is a sync primary that is neither degraded nor blocked
   (`repl_hold_` and not `repl_degraded_`).
2. Its link to the standby is LIVE: past the handshake, in this process,
   to the standby of this term. A standby of an older term refuses the
   handshake; a newer one fences this primary.
3. The link is not catching up from WAL files (no disk reader), and the
   standby lacks nothing that memory still holds unsent.
4. **The link's own acknowledged frontier** is at least the shard's
   durable LSN. `link_acked` counts only what this link has proven: the
   standby's verified position from the handshake, then its acks.
   - The sender's `acked_lsn`, which clients' OKs use, only ever grows and
     may have been earned by an earlier standby process. It is not used
     for readiness.
5. No regression is latched. A regression is a handshake whose position
   is below what this primary already counted as acknowledged: the
   standby lost acknowledged records (reseeded, wiped, restored). It
   clears only when 1 to 4 hold again on a later link.

**Why this is enough.** Take the moment a shard's checks hold. Every
record up to its durable LSN is durable on the standby. The standby acks
only what is durable there, and position 4 is that standby's own claim
on this link. After that moment, the shard is not degraded, so every
client OK it gives waits for the standby's ack of that record. Each shard
holds this from the moment it is checked. The main loop sends SYNCED
after all shards hold it. So once the witnesses record READY, the standby
holds every acknowledged write.

**Ongoing writes during catch-up** cannot produce an early SYNCED.
Check 4 compares against the durable LSN at the moment of the check, and
new writes raise it. Check 3 also keeps a shard not ready while a disk
reader runs. Catch-up ends when the standby's acks reach the durable LSN
of that moment. Writes still arriving then wait for their acks as usual
(the shard is sync and not degraded).

## 4. What invalidates readiness

- **The standby drops, and the primary would continue alone** (past
  `--replica-timeout-ms`). The existing path applies: DEGRADE must be
  chosen first, and that records NOT READY before any write is
  acknowledged on local durability.
- **The standby drops and comes back with the same data** (process
  restart, link flap) within the timeout. Readiness stays valid. The
  standby acked only records durable on its disk, so it still holds every
  acknowledged write, and the primary holds writes until it acks again.
- **The standby comes back without data it had acknowledged** (reseeded,
  replaced disk, restored directory). That is a regression (check 5).
  The primary then asks DEGRADE, which records NOT READY, without
  opening its own degrade gate. It owes SYNCED until checks 1 to 4 hold
  on the new link.
- **A seed** (a standby fetching a checkpoint from its primary's
  `--seed-port`) is served only while NOT READY is recorded for the
  primary's term. If READY is on record, the primary first asks DEGRADE,
  and the seed is refused with "retry" until the witnesses have recorded
  it. The seeding standby retries. So a standby can never become
  reachable as a standby with seeded, partial data while the witnesses
  still say READY. A rejoin after a failover always seeds in a new term,
  which is already NOT READY.
- **Term change.** A promotion records NOT READY for the new term. The
  new primary starts owing SYNCED. A primary whose term was replaced
  gets `NO term` and withdraws (unchanged).
- **Witness majority loss.** Nothing can be chosen, so no promotion and
  no READY. Unchanged.
- **A primary process starts** (first start, restart, or upgrade from a
  witness that recorded READY without proof). It does not know what is
  on record, and its new sender's acknowledged frontier starts at 0, so
  it could not detect a regression. It asks DEGRADE once, recording NOT
  READY, before anything else, and owes SYNCED.
- **A lost SYNCED reply.** The primary marks "NOT READY on record" false
  *before* it sends SYNCED, so if the witnesses chose READY but the reply
  was lost, a later seed or regression still asks DEGRADE first.

**The handshake position is durable.** A standby answers a primary's
handshake only once every record it has written is durable (its durable
LSN reaches its next LSN), since the primary counts that position as
acknowledged, for client OKs and for check 4. Before, it answered with
its next LSN even while its last records waited for their sync.

**Open (found by the independent review; not closed here).** A SYNCED
can be delayed inside a witness, queued behind another Paxos proposal on
that witness, after the primary gave up on it. If the standby then drops,
the primary is granted DEGRADE through another witness and acknowledges
writes alone, and the delayed SYNCED is then chosen on top of the grant
(same uuid, same term: the witness checks nothing else), the witnesses
hold READY while the primary runs alone. A crash of the primary then
promotes a standby without those writes. The window needs a SYNCED stuck
more than 1.5 s behind another proposal and a standby loss inside it; it
existed before B2 (the grant could be closed the same way). Closing it
needs SYNCED to name the state it follows, which is a change to the
witness request protocol, so per the work order it is **proposed, not
coded**:

> PROPOSED: DEGRADE and SYNCED replies carry the chosen state version
> (`OK v=N`). The primary remembers the version of its last answered
> DEGRADE and sends `SYNCED uuid term [group] after=N`; the witness
> refuses a SYNCED whose `after` is below the version of the latest
> DEGRADE chosen for that term. An old primary (no `after`) is accepted as
> today. Witness state format: unchanged (the version is already there).

**Residual (stated, not closed).** A standby whose data is lost without a
seed (started empty, or on a restored directory) reaches the primary as a
regression. Between that handshake and DEGRADE being chosen, a few
milliseconds (once this primary process has proven it; a new primary
process records NOT READY at start), a primary crash plus the witness's `--failover-ms` silence
could still promote it. This needs operator error: starting a standby on
the wrong or an empty directory with the same identity, which
EXPERIMENTAL_BASELINE Q9 already excludes. Closing it fully would need
the standby itself to know it was never confirmed. That is a protocol
change and is not proposed here.

## 5. Costs

- **After every failover, and at every primary start**, the group is not
  promotable until the new primary has a standby in sync and has said so.
  Until then a second failure makes the group unavailable, not wrong. This
  extends what Profile B's contract said ("after a failover, until a new
  standby is seeded") to: until it is seeded, caught up and proven.
- **No new state format.** SYNCED and DEGRADE are the existing verbs. The witness state file is unchanged. An older primary binary
  never sends SYNCED without a grant first, so with new witnesses its
  group would stay NOT READY: upgrade the witnesses and the nodes
  together.
- **Witness HA needs the primary started with `--witness`.** Only the
  primary can say SYNCED. A group whose primary runs without it is never
  READY, so its standby is never promoted (it fails safe). That was
  already required for a sync primary to run alone; the test clusters of
  `test_fail_closed.py` in witness mode now pass it.
- **Async groups (an R08-side effect, more conservative).** Before, an
  async group was READY from the moment the witnesses learned its
  primary. Now it is NOT READY until its primary (with `--witness`) has
  said SYNCED, which for async means its link is up (no LSN proof: an
  async primary acknowledges before its standby has anything). The rest
  of async promotion is unchanged: a live async standby of the same term
  is still promoted, and still may lack acknowledged writes (R08).
- **Liveness under load.** Check 4 compares against the durable LSN of
  the moment, so a shard under constant writes is ready only at an
  instant when its standby's acks have reached it. Measured at full client
  rate (2 threads x 64 in flight, loopback): SYNCED 0.48 s (`s3`) and
  1.12 s (`s7`) after the rejoined standby started. A degraded primary has no back-pressure, so a
  standby slower than its primary's write rate (or one restarting disk
  catch-up repeatedly, as with `--replica-retain-mib 1`) may never catch
  up: the group then stays NOT READY. That is unavailable for failover,
  never unsafe.
- The seed refusal is a new seed status (3, retry). An older standby
  binary treats it as a failed seed and exits, to be restarted.

## 6. Tests (work order B2 1 to 10)

These live in `tests/test_sync_readiness.py` and use the Pair of
`test_health.py` (two nodes, three witnesses, real processes) unless
noted.

1. Fail the primary immediately after the standby's seed starts: no
   promotion.
2. Fail it halfway through catch-up: no promotion.
3. Reach SYNCED, then fail the primary: the promotion succeeds with an
   exact state.
4. Restart a SYNCED standby on its own data (inside the replica
   timeout): READY on record throughout (polled). Then reseed it: NOT
   READY recorded before the seed is sent (in the primary's own log),
   READY again only once proven; then a failover is exact.
5. A stale SYNCED (an old term's) is refused, and promotes nothing.
6. Continuous writes at full rate during catch-up: never READY while the
   standby is behind (sampled every 50 ms: the check proves no early
   READY was *seen*, not that none existed between samples; s7 kills the
   primary at the SYNCED itself); READY while the writes continue; then
   exact.
7. Fail the primary immediately after SYNCED: every acknowledged write
   is present.
8. Two consecutive failovers with a reseed between them: exact state
   (`test_baseline_retry.py q5_kill_kill`, and `q5_catchup_race`, which
   now passes because nobody is promoted while catching up).
9. Migration, then rejoin, then failover: D1 and D7 hold
   (`test_fail_closed.py f19`: sync groups, witness HA, a completed move,
   failover, rejoin by seed, failover again).
10. Witness majority loss while not ready: no promotion.

## 7. Causal order of NOT READY and READY (PR #49 review)

**Status: DESIGN, 2026-11-05** (work order
`handoff/PR49_Review_Handoff.txt`). This closes the window of section 4
("Open"): a SYNCED delayed inside a witness was chosen after a later
DEGRADE and restored READY while the primary acknowledged writes alone.
It needs **no new durable state**: the witness state file, its Paxos
machine and the state body `uuid term degraded primary` are unchanged.
It adds two request verbs and changes how DEGRADE is chosen.

### 7.1 Why the hole exists

The HA state of a group is one Paxos machine, `ha/G`. Version n is the
value chosen in instance n, and every proposal for instance n is computed
from the value chosen in instance n-1. Two things let a stale SYNCED win:

1. **SYNCED names only uuid and term.** Its check passes against any
   state of that term, so a SYNCED computed from the primary's proof at
   time t1 is still accepted after a DEGRADE at t2 > t1.
2. **DEGRADE on a NOT READY state chooses nothing.** `propose` returns
   OK at once when the wanted body equals the current one, so a DEGRADE
   while NOT READY leaves the version where it was. A SYNCED computed
   against that version, held in a witness's queue or accepted by a
   minority in the next instance (and adopted by the next proposer, as
   Paxos requires), still applies on top of it.

A request is held in a witness by its serial proposal lock: `propose`
reads the base version only once it holds the lock, so a SYNCED queued
behind other proposals is evaluated against whatever state exists when
it runs, however late.

### 7.2 The rule

- **NOT READY always gets a fresh version.** Every DEGRADE (old verb) and
  NOTREADY (new verb) chooses a new instance holding NOT READY for the
  term, even when the state is already NOT READY. The reply to NOTREADY
  names it: `OK v=N`, where N is the version of the chosen value that
  holds this NOT READY (the instance this request won, or the one a
  concurrent equal NOT READY won: either way N is above every version
  the primary had seen when it asked).
- **READY names its causal base.** The primary sends
  `READY uuid term [group] after=N`. N is the version of the most recent
  NOTREADY this primary process had answered OK *before it read the
  proof* it is reporting (section 3). The witness's change function,
  evaluated against the base state B of the instance it proposes in:
  - B.uuid empty: `NO not learned yet`. B.uuid != uuid: `NO another space`.
  - B.term != term: `NO term T was replaced by U` (unchanged).
  - B is READY (degraded false): `OK v=B.version`, nothing chosen. The
    state already says what READY wants; no transition happens, so no
    stale base can do harm.
  - B is NOT READY and **B.version == N**: choose READY (version N+1).
  - B is NOT READY and B.version != N: `NO not ready since v=B.version`.
    Some NOT READY transition for this group and term was chosen after
    the primary's base: the proof does not cover it.
- **SYNCED (old verb) is refused** by a new witness (after the space and
  term checks, so a stale-term SYNCED still reads `NO term`): `NO SYNCED
  has no causal base; READY ... after=N is required`. It can no longer
  move a group to READY.

**Why `B.version == N` is exact, not just sufficient.** Within one group
and one term, only four kinds of value are chosen: learning (once per
group, NOT READY), promotion (starts a term, NOT READY), NOT READY
(DEGRADE/NOTREADY) and READY. So when B is NOT READY in term T and
B.version > N, a NOT READY was chosen after N in T (a READY in between
and a later NOT READY is still a later NOT READY): exactly what must
refuse. B.version < N is impossible for an honest primary (N came from
this machine) and is refused too.

**Why it is safe.** Let the primary's proof be read at time t, after it
received `OK v=N`. Any write acknowledged on local durability alone was
acknowledged under a grant, and a grant is a NOT READY that the primary
received before opening its gate. A grant received before t is version
<= N, and the proof covers every write durable at t (section 3). After
t, a shard acknowledges alone only after a new grant, which is a new
version M > N, chosen before the gate opens. READY after=N is chosen
only in instance N+1, so either it is chosen there (before M exists:
M is then at least N+2, and the state ends NOT READY) or instance N+1
holds something else and READY after=N can never be chosen. In every
order, the latest chosen value is NOT READY whenever a write exists
that the standby may lack.

**Unrelated proposals do not block legitimate READY.** Versions are per
machine (`ha/G`): placement, mint leases and other groups are separate
machines whose decisions never move `ha/G`'s version (tools/paxos.h).
A failed promotion proposal chooses nothing. So between a NOTREADY and
the READY that follows it, the version moves only if another NOT READY,
a READY or a promotion of this group is chosen, and each of those must
refuse (or makes READY unnecessary).

### 7.3 The primary

- `ready_base` (in memory, this process): 0 at start, after a promotion,
  and after a refusal `NO not ready since`; set to N by every NOTREADY
  answered `OK v=N`.
- With witnesses, every request that records NOT READY uses NOTREADY:
  the grant to run alone, a regression, a seed, the start. While
  `ready_base` is 0 and READY is owed, the primary asks NOTREADY first
  (this replaces "once at start", and adds one Paxos round after a
  promotion: the promotion's NOT READY version is not known to the new
  primary).
- READY is sent only with `after=ready_base`, from a proof read after
  that NOTREADY was answered: the shards are read again after any
  NOTREADY in the same loop iteration (the loop already re-reads them).
- A lost READY reply is retried as is: idempotent (7.2: READY returns OK
  if already READY; if a NOT READY came between, the retry is refused
  and the primary starts again from NOTREADY).

### 7.4 The ten questions of the review

1. **Which version does DEGRADE return?** NOTREADY returns the version
   of the chosen value holding its NOT READY, always a new one. Old-verb
   DEGRADE also chooses a fresh version but answers plain `OK` (older
   primaries compare the reply to `OK`).
2. **Can replies differ, and how does the primary reconcile them?** Each
   request goes to one witness (the next one only if no answer). Two
   NOTREADY answered by different witnesses give different versions; the
   primary keeps the last one answered. A larger N is never wrong: any
   NOT READY chosen after the one the primary knows makes its READY
   refused, which only costs another NOTREADY round.
3. **The exact predicate for READY after=N:** same uuid and term, and
   either the base is READY (no-op OK) or the base is NOT READY with
   version exactly N.
4. **Unrelated proposals:** separate machines, separate versions (7.2).
5. **Global or per group?** Per group (`ha/G`'s Paxos version).
6. **The causal base after a primary restart:** not recovered. A new
   process starts with `ready_base` 0 and asks NOTREADY before any
   READY, so any READY still in flight from the old process names an
   older version and is refused.
7. **A lost DEGRADE reply:** the primary did not open its gate, so no
   write was acknowledged under that grant. It asks again; the retry
   chooses another fresh version and returns it. The lost one is just an
   extra NOT READY.
8. **A lost SYNCED (READY) reply:** retried with the same `after=N`. If
   the first one was chosen, the state is READY and the retry answers OK
   without choosing anything; if a NOT READY came in between, the retry is
   refused and readiness restarts from NOTREADY.
9. **Mixed old and new.**
   - New witnesses, old primary: DEGRADE works (fresh versions, reply
     `OK`); SYNCED is refused, so the group never becomes READY and is
     never promoted. Fails safe: failover unavailable, durability intact.
   - Old witnesses, new primary: NOTREADY and READY are unknown verbs
     (`NO unknown request`). The primary tries the next witness and
     logs that the witness is too old; with no new witness it gets no
     grant (durable writes wait on a lost standby) and never READY.
     Fails closed.
   - Mixed witnesses, new primary: safe. Only new witnesses accept its
     requests; an old witness never proposes READY for it (it only
     proposes from SYNCED, which the new primary never sends) and old and
     new witnesses check promotions against the same chosen state.
10. **Can an old primary without `after` re-open the race?** Only through
    an old witness, which accepts its SYNCED unconditionally: that is the
    pre-fix system. New witnesses refuse it. So the protected Profile B
    claim needs every witness of the group upgraded; the order is
    witnesses first (old primaries then fail safe), then nodes.

### 7.5 What it does not change

The residual of section 4 (an operator-started wrong, empty or restored
standby directory, between its handshake and NOT READY being chosen) is
not closed: it does not involve a delayed READY. Still excluded by
EXPERIMENTAL_BASELINE Q9. No witness state format change, no Paxos
change other than letting a proposal ask for a fresh instance and report
the version it chose.
