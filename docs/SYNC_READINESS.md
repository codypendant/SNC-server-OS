# Sync readiness: when a standby may be promoted (B2)

**Status: IMPLEMENTED 2026-11-05** (work order
`handoff/PR47_PR48_Blocker_Fix_Handoff.txt`, B2). This fixes BLOCKER B2 of
[BASELINE_READINESS.md](BASELINE_READINESS.md) section 3. It covers Profile
B (sync replication with witness HA) only. It does not change async
promotion policy (R08), and it changes no format: the witness state, its
Paxos machine and the replication wire are as before.

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
and only a standby with the same uuid and term.

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

**Residual (stated, not closed).** A standby whose data is lost without a
seed (started empty, or on a restored directory) reaches the primary as a
regression. Between that handshake and DEGRADE being chosen, a few
milliseconds, a primary crash plus the witness's `--failover-ms` silence
could still promote it. This needs operator error: starting a standby on
the wrong or an empty directory with the same identity, which
EXPERIMENTAL_BASELINE Q9 already excludes. Closing it fully would need
the standby itself to know it was never confirmed. That is a protocol
change and is not proposed here.

## 5. Costs

- **After every failover, and at first start**, the group is not
  promotable until the new primary has a standby in sync and has said so.
  Until then a second failure makes the group unavailable, not wrong. This
  is what Profile B's contract already says for "after a failover, until a
  new standby is seeded".
- **No new wire or state format.** SYNCED and DEGRADE are the existing
  verbs. The witness state file is unchanged. An older primary binary
  never sends SYNCED without a grant first, so with new witnesses its
  group would stay NOT READY: upgrade the witnesses and the nodes
  together.
- **Witness HA needs the primary started with `--witness`.** Only the
  primary can say SYNCED. A group whose primary runs without it is never
  READY, so its standby is never promoted (it fails safe). That was
  already required for a sync primary to run alone; the test clusters of
  `test_fail_closed.py` in witness mode now pass it. An async primary with
  `--witness` says SYNCED once its link is up, as before (R08 unchanged).
- **Liveness under load.** Check 4 compares against the durable LSN of
  the moment, so a shard under constant writes is ready only at an
  instant when its standby's acks have reached it. Measured at full client
  rate (2 threads x 64 in flight, loopback): SYNCED 0.5 to 1.3 s after the
  rejoined standby started. A degraded primary has no back-pressure, so a
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
4. Restart a SYNCED standby on its own data: it stays READY, and a
   failover is exact. Reseed it: NOT READY until it is proven again.
5. A stale SYNCED (an old term's) is refused, and promotes nothing.
6. Continuous writes at full rate during catch-up: never READY while the
   standby is behind; READY while the writes continue; then exact.
7. Fail the primary immediately after SYNCED: every acknowledged write
   is present.
8. Two consecutive failovers with a reseed between them: exact state
   (`test_baseline_retry.py q5_kill_kill`, and `q5_catchup_race`, which
   now passes because nobody is promoted while catching up).
9. Migration, then rejoin, then failover: D1 and D7 hold
   (`test_fail_closed.py f19`: sync groups, witness HA, a completed move,
   failover, rejoin by seed, failover again).
10. Witness majority loss while not ready: no promotion.
