# Legacy data directories: role adoption (upgrade hardening)

Status: implemented (2026-11-04), for review. Closes residual risk 6 of
[MIGRATION_FAIL_CLOSED.md](MIGRATION_FAIL_CLOSED.md) section 20, as the
review of PR #40 (item 17) ordered before S1.

## 1. The hazard

PR #40 added `DIR/role` (`primary` or `standby`) and the superblock flag
`kSbRoleTracked`. A directory whose role file is missing or unreadable
*after* tracking exists runs the start barrier as a possible standby
prefix. But a directory written by a binary from before role files has
neither the file nor the flag, and PR #40 started it as a primary with no
barrier. A directory that was a **standby** under the old binary has
exactly the same state. Restarted on the new binary without `--standby`
(a wrong flag, a supervisor's default command line), it would serve as a
primary from a possibly lossy prefix and skip the barrier: the D1/D7
protection bypassed by a startup mistake.

Rule (review, item 17): **an unknown legacy role fails closed.** A
missing role file plus a missing `kSbRoleTracked` is not evidence of a
primary.

## 2. Options considered

| | What | Verdict |
| --- | --- | --- |
| B | Infer the role from another durable source | **Rejected: no unambiguous source exists.** An old standby keeps the same superblock (its primary's space UUID, the same term), the same WAL record format (records received keep the primary's LSN and CRC) and the same checkpoints; nothing on disk says which side wrote it. A heuristic ("its WAL ends earlier") cannot be right in both directions, and a safety boundary cannot rest on one |
| C | Prove the role from the control plane or replication config | **Partly usable, not sufficient alone.** The placement names a group's members but not which is primary; the witnesses keep no per-group role or term that can be asked (their verbs: DEGRADE, SYNCED, PLACEMENT, LEASES, Paxos). The group's other members can be asked (`HEALTH`), which can *disprove* a primary claim but cannot prove one |
| A | Explicit, one-time operator adoption, refusing ordinary startup until then | **Chosen**, with C's evidence as a check on the claim and the start barrier still run |

## 3. Behavior (as built)

A directory is **legacy** when it has a superblock, no `DIR/role` file
and no `kSbRoleTracked`. Only a legacy directory is affected; new and
tracked directories behave exactly as in PR #40.

| Start of a legacy directory | Result |
| --- | --- |
| no flag (as a primary) | **refused**, before anything is written ("legacy data directory ... start it once with --standby [--adopt-role standby] ... or --adopt-role primary"); control plane up or down |
| `--standby` (alone, or with `--adopt-role standby`) | role `standby` written (atomic, fsynced) and the superblock written with `kSbRoleTracked`, both before any shard starts or replication record arrives; then an ordinary standby. A later promotion (in place or offline) runs the start barrier as for any former standby |
| `--adopt-role primary` | **checked**, then started **through the start barrier**; `kSbRoleTracked` and role `primary` only once the barrier's re-seal is durable (below) |
| `--adopt-role standby` without `--standby` | refused (adoption names what the directory is and how it runs; a standby's directory is promoted later through the barrier, not by adoption) |
| `--adopt-role primary` with `--standby` | refused |

`--adopt-role` on a directory that is not legacy: refused if it is new
("nothing to adopt") or its recorded role differs ("a recorded role is
never overridden"); a matching role is a no-op, so a script may keep the
flag for one more restart. Every refusal happens before anything that
affects the directory's role classification is written (`--control`
still refreshes the stored placement, as on any start).

### 3.1 The check on `--adopt-role primary` (evidence, option C)

Refused, with the reason, when:

- the control plane did not answer (the node started from its stored map);
- a member of the group has no admin port in the placement, so it cannot
  be asked;
- any other member of the group (from the placement; also the `--rejoin`
  peer) does not answer `HEALTH` (`STATUS` for an older binary): it may be
  the primary this directory was a standby of;
- with `--control`, the directory holds another space than its group's
  (checked before the barrier, so it is never labeled);
- a member answers without a readable role and space UUID;
- a member of the same space answers as a primary (`HEALTH` role `p` or
  `n`) or is promoting (`MOVE ROLE` answers `m`: `HEALTH` still says `s`
  while a standby is inside its promotion): this directory is not the
  group's primary;
- a member of the same space is fenced (`f`: it was this space's primary)
  at this directory's term or a later one;
- a member of the same space reports a newer term than this directory's
  superblock: the group failed over after this directory was written, so
  it is stale.

With no member to ask (no `--control` placement naming one and no
`--rejoin` peer), the adoption is the operator's word alone, and the node
logs a warning saying so (section 6). A refusal exits with status 1 and
the reason (`snc_server: not started: ...`). With `--rejoin`, the rejoin
step is skipped under `--adopt-role primary` (the check asks the same
peer, and refuses before the directory is touched).

The check is a snapshot: it finds evidence *against* the claim at that
moment. It does not stop the witnesses from promoting the standby a
moment later; that race is the same as for any restart of a tracked
primary, and the newer term fences the loser (C7, R08).

Only then does the start continue, and it is still the start barrier:
the placement read linearizably (with `--control`: refused if the control
plane does not answer; without it there is no placement to read, section
6.1), a slot this
group is moving away re-sealed, the re-seal durable, a second sync, and
only then `DIR/role` = `primary`, then `kSbRoleTracked` in a new
superblock generation. As for any barrier start, the adopting node may
serve independently proven unaffected slots while the migration barrier
finishes; the affected slot remains non-serving, and the directory is not
declared primary until barrier durability completes (a crash before that
leaves it legacy, below).

### 3.2 Crash safety

Until role `primary` is written, the adoption writes nothing that changes
the classification: the superblock written during recovery keeps
`kSbRoleTracked` clear. A crash anywhere before the end of the barrier
(including with the barrier held open, `ROLE m`) leaves the directory
legacy, so the next ordinary start is refused again and a new adoption
re-runs the check and the barrier. A crash after role `primary` and before
the new superblock leaves a directory with role `primary` and no flag,
which reads as a primary's (the role file is authoritative when present):
the barrier has completed by then. The standby direction is the fail-safe
one and is made durable first.

### 3.3 In-place promotion wording (review item 8)

As PR #40 built it and the review accepted: **the node may serve
independently proven unaffected slots while the migration barrier
finishes; the affected slot remains non-serving, and the directory is not
declared primary until barrier durability completes.** The whole node
does not wait to serve.

## 4. Rollout

No compatibility mode was added: the protected behavior is the only
behavior of this binary, so the upgrade itself is the cutover. The cost
is one explicit start per pre-upgrade directory:

1. Upgrade each group's standby first: start it with `--standby` (as
   before). Nothing else is needed.
2. Then each group's primary: start it once with `--adopt-role primary`,
   with the control plane up and the group's standby running (it is
   asked). Later restarts need no flag.

A primary upgraded before its standby is refused while the standby does
not answer; a directory the operator cannot place is started with
`--standby` (the safe direction) and promoted later through the barrier.

## 5. Tests (`tests/test_fail_closed.py`)

`make_legacy` turns a stopped node's directory into what a binary from
before role files leaves (role file removed, superblock flags 0, CRC-32C
recomputed); `compat` checks the same path with directories written by a
real older build (`--old`, main at 4c92093).

| Requirement | Where |
| --- | --- |
| L1 legacy primary, explicit adoption, serves | `legacy_primary` (standby answering; accounts served, a deposit acked; role `primary`, tracked); `compat` (old-build directories) |
| L2 legacy standby cannot accidentally serve as primary | `legacy_standby` (adoption as primary refused: its primary serves); `compat` (refused: its primary does not answer) |
| L3 legacy standby without `--standby` fails closed | `legacy_standby`, `compat`; also a legacy primary without the flag (`legacy_primary`) |
| L4 control plane down during an ambiguous start | `legacy_primary`, `legacy_standby`: refused without adoption and with `--adopt-role primary`; `compat` |
| L5 adoption refused on contrary evidence | `legacy_standby` (a member serves as primary); `legacy_stale` (a member mid-promotion, `ROLE m`; a member serves; a member does not answer; a member at a newer term: "this directory is stale") |
| L6 standby adoption durable before replica service | `legacy_standby` (`--standby` alone and with `--adopt-role standby`: role `standby` and the flag on disk when it answers `s`; written before shards start, `Engine::recover`); then promoted offline through the barrier, refused while the control plane is down |
| L7 crash during adoption | `legacy_primary`: `--debug-barrier crash` (exit 9) and a kill with the barrier held: still no role, no flag; the next ordinary start refused |
| L8 normal behavior after adoption | `legacy_primary`: plain restarts (also with the control plane down), a matching `--adopt-role` a no-op, a different one refused, a lost role file runs the barrier |
| L9 compat no longer relies on missing role => primary | `compat` rewritten: every pre-upgrade directory is refused without adoption; standbys via `--standby`, primaries via `--adopt-role primary` |

L6's order (role and tracking written before any shard starts) is shown
by the code (`Engine::recover`: `write_role`, then the superblock, then
the runtime), the test checks the result. Not tested: a member with no
admin port in the placement, a fenced member, a directory of another
space, a crash between role `primary` and the superblock flag (it reads
as a primary's, the barrier having completed), and adoption without a
control plane (the warning only).

## 6. Residual risks

1. **Non-control-plane deployments** (`--cluster` lists or a standalone
   pair with `--replica-of`, no `--control`): there is no placement naming
   the group's members' admin ports, so `--adopt-role primary` asks only a
   `--rejoin` peer if one is given; otherwise it is the operator's
   assertion. Without a control plane there are no slot moves, so the
   barrier has nothing to re-seal there; the risk left is R08's (a lossy
   prefix serving ordinary data), not D1/D7.
2. **Both labels swapped.** If an old primary's directory is started with
   `--standby` and its old standby's with `--adopt-role primary`, the
   standby answers `s` at the same term and the adoption is accepted:
   the evidence disproves a claim, it cannot prove one. Two wrong
   explicit flags are needed; a single mistaken or missing flag is caught.
   (Comparing durable WAL positions with a same-term standby would catch
   this too; not built, to keep the slice small.)
3. **A member that is down** (or is itself a legacy directory not yet
   started) cannot be asked; the adoption waits for it, or for the
   placement to drop it. That is availability, not safety.
4. A rollback to an older binary and back can leave a stale `primary`
   role file on a directory that served as a standby meanwhile (older
   binaries do not write it). Unchanged from section 20; rollback stays
   manual and explicitly unsafe.

## 7. Runs, review and QA (2026-11-04, this VM: 4 vCPU, loopback)

- Fresh build directories after every review and QA fix:
  RelWithDebInfo `ctest` 20/20 (`test_fail_closed`, now 27 scenarios
  with the three `legacy_*`, 221 s); ASan+UBSan (Debug,
  `-fno-sanitize-recover=undefined`) 20/20; TSan (Debug,
  `halt_on_error=1`) 20/20. The Python tests run the sanitized servers
  and witnesses; none reported.
- `compat` and `f7b` with `--old` (main at 4c92093, before role files):
  ok.
- **Independent review** (a separate reader; nothing run): no blocking
  finding; every path by which a legacy directory could start as a
  primary without adoption checked (offline start, in-place promotion,
  `--rejoin`, seeding, in-memory). Fixed from it: a standby inside its
  promotion answered `HEALTH` role `s` (now also asked `MOVE ROLE`, which
  says `m`; tested); a fenced member at this directory's term was not
  counted against the claim; no warning when there is no member to ask;
  an unreadable answer was skipped; `--rejoin` could move the directory
  aside before refusing; refusals aborted the process (now exit 1);
  two test assertions matched the wrong message. Kept as residual: the
  swapped-labels case and no WAL-position comparison (section 6).
- **QA** (a second reader, against the work order): L1-L9 all covered;
  nothing out of scope built; preservation items intact. Fixed from it:
  a directory of another space was refused only after its barrier had
  labeled it (now refused in the check); wording on the no-control-plane
  case, on serving during the barrier, and on what a refusal writes; the
  refused-start helper now asserts a legacy directory stays legacy.
