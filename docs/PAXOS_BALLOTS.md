# Paxos ballots after a witness restart (F8)

**Status: CHARACTERIZATION AND DESIGN, for review (2026-11-05).** Work
order `handoff/PR50_Review_Handoff.txt`. Nothing in the protocol or the
state file changed; the only code added is test hooks behind
`snc_witness --debug 1`. Evidence: `bench/results/paxos-ballots-20261105/`.
Test: `tests/test_paxos_ballots.py` (in `ctest`). Suites on the hook
commit: release `ctest` 30/30; ASan+UBSan and TSan
`test_paxos_ballots`, `test_witness`, `test_ready_order` 3/3 each, zero
sanitizer reports (`ctest_*.txt` in the evidence folder).

## 1. Result

| Finding | Reproduced | Can two values be chosen? | Proposed disposition |
| --- | --- | --- | --- |
| **F8**: a restarted witness reuses ballots it already used (clock stepped back, or its counter had run ahead of the clock) | **Yes** (C1: 149 ballots at or below B generated again, B itself among them) | **No.** The acceptors' persisted promises refuse a reused ballot in any instance where it was promised; the proposer then takes a higher ballot and adopts what was accepted (C2a, C2b, C2l). Proof in section 4 | **Outcome A**: downgrade to a liveness note; keep C1 to C3 as regression tests. No ballot persistence needed |
| **F9 (new)**: a witness that loses its acceptor state (state file deleted, or restored from an older copy) and comes back in place | **Yes** (C4a restored, C4b deleted) | **Yes.** X chosen at version 1 by witnesses 0 and 1; witness 1's state restored to before X; witness 0 down: Y chosen at version 1 by witnesses 1 and 2. READ: witness 0 `1 X`, witnesses 1 and 2 `1 Y` | **Outcome B** for the baseline (excluded history plus a guard, section 6). A real fix needs new durable witness state: design in section 7, for review |

F9 does not involve ballots: persisting the ballot counter would not
prevent it (Y used a ballot higher than any used before). It is the
general Paxos requirement that an acceptor never forgets a promise or an
acceptance, which this implementation meets only while the state file
survives intact.

## 2. How ballots are built (tools/paxos.h)

- `ballot = (counter << 6) | id`. `id` is the witness's `--id`, its
  index in `--peers`; `snc_witness` refuses more than 63 peers or an id
  that does not name its own port. The low 6 bits keep two witnesses'
  ballots apart.
- `counter = max(counter + 1, wall clock in ms)` (`system_clock`), taken
  under the node's mutex for every PREPARE (`propose` and `read_linear`,
  one new ballot per attempt). A NACK raises it: `counter =
  max(counter, nacked >> 6)` (both phases of `propose`; `read_linear`
  only reads NACKs to its PREPARE).
- **Scope:** one counter per witness process, shared by every machine
  (`ha/G`, `placement`, `leases`, and the test machines).
- **Persistence:** none. The counter lives in memory and starts at 0, so
  a new process's first ballot is the wall clock.

## 3. What survives a restart

The state file (`--state`) holds, for every machine: the chosen value,
the open instance, the ballot promised in it, and the ballot and value
accepted in it. It is written to a temporary file, fsynced, renamed over
the old one, and its directory fsynced, **before** any reply that
depends on it (PROMISE, ACCEPTED, a NACK after learning, COMMIT, and the
proposer's own learning). A crash between the write and the reply only
loses the reply, which is the safe direction. One exception, harmless to
safety: a `NO behind` reply after learning a newer chosen base does not
save it first, so that learned value can be lost in a crash (it is
learned again from the next proposal or read).

So after a restart, every promise and acceptance this witness ever
answered is still on its disk. Only the proposer's counter is lost.

## 4. Why a reused ballot cannot choose a second value

The usual Paxos proof assumes ballots are unique, but it uses that only to
get this property:

**P. For each machine, instance v and ballot b, the proposers send at most
one value in `ACCEPT m v b`.**

Here P holds without unique ballots:

1. A proposer sends `ACCEPT m v b` only after a majority answered
   `PROMISE` to `PREPARE m v b` (`propose` and `read_linear` both check
   this). It computes one value from those promises and sends it; a
   retry takes a new ballot.
2. An acceptor answers `PROMISE` at `(v, b)` only if `b > promised` in
   instance v (strictly greater), and records `promised = b` durably
   before replying. While v is its open instance, `promised` never goes
   down: ACCEPT sets it to a ballot at least as high, and the only reset
   is moving to a later instance, after which any request for v gets
   `LATE` or `NO behind` (refused either way). An acceptor enters
   instance v only after v-1 is chosen: through a chosen base (PREPARE,
   COMMIT, a read) or an ACCEPT for v, which a proposer sends only after
   a phase 1 on a chosen base.
3. So each acceptor promises `(v, b)` at most once in its lifetime, as
   long as its state file survives. Any two majorities share an acceptor,
   so at most one phase 1 at `(v, b)` ever succeeds, whoever runs it,
   whether a restarted process reusing b or two witnesses misconfigured
   with the same `--id`. By step 1, that gives P.

With P, the standard argument applies unchanged. Once a value is chosen
at `(v, b)`, any later successful phase 1 at a higher ballot hears it
from at least one acceptor of the choosing majority. The proposer adopts
the value accepted at the highest ballot, and by P that is unique per
ballot.

**What P rests on**, shown by C2x:

- The acceptor takes an `ACCEPT` at a ballot *equal* to its promise,
  whatever the value.
- A proposer that skipped phase 1 could therefore overwrite X with Y at
  the same ballot.
- No proposer in this code does that. A second phase 1 at B is refused
  (`NACK B`) by every acceptor that promised B.

**What a reused ballot costs:**

- It costs liveness only: the first attempt at B is refused, and the NACK
  moves the counter above B. In C2a and C2b the restarted witness needed
  three ballots: B refused, a higher one that adopted and finished X, then
  one for Y.
- After a large clock step back, the first PREPARE in each instance is
  refused once.

## 5. Tests (`tests/test_paxos_ballots.py`)

Three real witnesses, `--debug 1`, test machines `debug/...` only. The
test hooks:

- `--debug-clock-offset-ms N` and `DEBUG CLOCK-OFFSET-MS`: shift the
  clock ballots are taken from.
- `DEBUG SET-COUNTER`: forces exactly B.
- `DEBUG NEXT-BALLOT`.
- `DEBUG PROPOSE debug/NAME BODY`: proposes a value and answers with
  every ballot it used.
- Raw `PREPARE` and `ACCEPT` stand in for a proposer that crashed
  mid-round.

| # | History | Result |
| --- | --- | --- |
| C3 | Ordinary restart, clock unchanged | The first ballot after the restart is above every ballot used before |
| C1 | X chosen at B (COMMIT dropped, so the acceptors hold it accepted, instance open); the proposer restarts with its clock 200 ms below B's counter | **Reuse occurs**: 149 of the 150 ballots it generated were at or below B, and B itself was among them |
| C2a | B promised by witnesses 0 and 1; X accepted at B by witnesses 1 and 2 (chosen); witness 0 crashed before learning it, restarts rolled back, proposes Y with exactly B, **same instance** | B refused by all three; the next ballot adopts X (version 1); Y is chosen at version 2. One value per version |
| C2b | B promised by witnesses 0 and 1; X accepted only by witness 0 (the ACCEPT to witness 1 lost); witness 0 restarts rolled back, proposes Y with B | Same: B refused, X adopted and chosen at version 1, Y at version 2 |
| C2l | As C1, but the proposer had learned X before crashing | B goes to instance 2, where it was never used: Y chosen at version 2 at once |
| C2x | Not reachable by this code: a second phase 1 at B, then ACCEPT B Y without a majority of promises | The second phase 1 is refused by the acceptor that promised B. Only a proposer ignoring that could send the ACCEPTs, and they would be taken |
| C4a | X chosen by witnesses 0 and 1 (witness 2 down); witness 1's state file restored to a copy taken before X; witness 0 down; witness 1 proposes Y | **Two values chosen at version 1**: witness 0 `1 X`, witnesses 1 and 2 `1 Y` |
| C4b | As C4a with witness 1's state file deleted | The same: two values at version 1 |

One test-design note from C4: a SIGSTOPped witness still accepts TCP
connections, and on SIGCONT it serves the requests from its socket
backlog late. That is a delayed message, which Paxos tolerates; to keep a
witness out of a decision, the test kills it.

## 6. F9 for the baseline: excluded history and guards (proposed, not built)

**Which histories produce F9.** A witness restarted in place with acceptor
state older than what it had answered:

- a disk replaced and the witness restarted with an empty state file;
- `--state` pointing at a new or wrong path (a typo; a new mount);
- the state directory restored from a backup, or the witness VM or
  container restored from a snapshot;
- a state file copied from another witness.

**What else it needs.** To actually choose a second value, a proposer
must reach a majority that includes the forgetful witness but no witness
that accepted or learned the first value. With 3 witnesses that means
the forgetful one plus one that never saw it, while the others are down
or slow. C4 forces this. In a live cluster it is a narrow but real
window, and its effect would be two different HA states for one group
(two terms or two primaries), or two placements.

**Not involved:** a normal restart, a crash, a clock step (with the disk
intact), and NTP slewing or stepping.

**Proposed for Profile B (outcome B):**

- **E1 (exclusion):** a Profile B run never restarts a witness with
  acceptor state older than it had, or with none. That rules out
  restoring a witness state file, directory, VM or container snapshot,
  and starting a known witness on an empty or different `--state`.
  Replacing a witness's disk means replacing the witness through the
  recovery procedure of section 7, once that is reviewed and built. Until
  then, stop the run.
- **G1 (guard, CLI only, no state format change):** `snc_witness`
  refuses to start when its `--state` file does not exist, unless
  `--new-state 1` is given. First starts and test clusters pass the flag.
  This catches deleted files and wrong paths. It cannot catch a restored
  copy, which looks like a witness that was down for a while.
- **G2 (harness):** the campaign harness passes `--new-state 1` only when
  it creates a campaign's witnesses. The manifest records each witness's
  state path, and the harness refuses a run whose witness state file is
  missing or replaced (a different inode) since the campaign began.

## 7. The real fix for F9 (design only; needs new durable witness state)

A witness that comes back without trustworthy state must not take part
in any instance where it might have promised or accepted before. The
operator starts it with `--recover 1`, or with `--new-state 1` into an
existing cluster. Then:

1. Before answering any Paxos request, it reads from a majority of the
   whole cluster drawn from the **other** witnesses (with 3, both others;
   its own empty or old reply must not count) every
   machine they hold state for. That needs a new read-only verb listing
   machines, including machines with chosen version 0 but a promise or
   acceptance on disk. For each machine it records `H(m)`, the
   **maximum** version reported.
2. It writes durably, before listening, a new state line per machine:
   `recovering m H(m)`.
3. It answers `PREPARE` and `ACCEPT` for machine m only in instances
   above `H(m) + 1`, refusing the rest, and only for machines it holds a
   `recovering` line for, or machines that first appear after recovery
   began (none of its old promises can be in those). It still answers
   READ and learns from COMMIT and from bases.
4. Why this is safe: the witness could have promised or accepted in
   instance v only after a phase 1 for v that reached a majority Q.
   Every member of Q learned v-1 from that PREPARE's base and saved the
   machine. (A promise from a phase 1 that failed is harmless: no ACCEPT
   follows it.) Q minus this witness (a majority less one) and the read
   set (a full majority, all of them other witnesses) overlap among the
   N-1 others: (N+1)/2 - 1 + (N+1)/2 > N - 1. So
   some reply reports at least v-1 for m, which gives v <= H(m) + 1. Using
   the "highest a majority agrees on" instead would be wrong: a chosen
   value whose COMMIT was lost is reported by nobody. (Independent
   review finding.)
5. Once its learned chosen version for m passes `H(m) + 1`, the
   `recovering` line for m is dropped and the witness takes part again.
6. Liveness: until then, the cluster needs a majority without it (2 of
   the other 2 with 3 witnesses).

**Crash ordering:**

- A crash before step 2 changes nothing, and the witness has answered
  nothing yet.
- A crash after step 2 restarts in recovery with the same lines.
- Repeated restarts keep the lines until each is cleared.

**Compatibility and upgrade:**

- The new line type is ignored by an old binary's loader (unknown keys
  are skipped), so downgrading a recovering witness would silently drop
  the protection. The format therefore needs a version line that old
  binaries refuse. That is the "new durable witness format" the work
  order says to return before building.
- Old directories load unchanged.
- Mixed versions: old peers answer the machine-list verb with
  `NO unknown request`, and recovery then waits for a majority of new
  witnesses.

**Backup and restore under this design:**

- Restoring a witness is allowed only through `--recover 1`.
- A restored witness started without it is still undetectable locally.
  E1 stays the rule for that.

**Not proposed:** a durable ballot allocator. Section 4 shows it adds no
safety. Its only gain would be skipping one refused PREPARE after a
clock step back.

## 8. Limits and review findings

The independent review of this document found no counterexample to
section 4. It corrected section 7 (step 4's argument; the read must
exclude the recovering witness, take the maximum, and list machines with
only a promise). It also found these, reported here and not fixed
(characterization only):

- **F10, legacy state lines.** A state file line whose machine name is
  all digits is read as a pre-machines line and renamed `ha/0`
  (`load()`, for files from before machines had names). `handle()`
  accepts any machine name and `save()` writes every entry, even one
  only a READ created. So a stray `READ 5` request, followed by any
  save, writes `chosen 5 0 -`. On restart that line loads as `ha/0`
  version 5, if the file has no `ha/0` lines of its own. That happens
  for a witness that watches other groups only. No code in this repo
  sends such a READ. Fix options: do not create entries on READ, or
  convert legacy lines only in files with no machine names at all.
- **F11, duplicate peers.** `--peers` with the same host:port twice is
  not rejected, so one acceptor can count twice toward a majority. A
  configuration check would close it.
- **Durability edges.** If the state directory's `fsync` fails, or the
  directory cannot be opened for it, the failure is ignored. A rename
  lost to a crash then loses a promise, which is F9. Making it fatal,
  like the file's own `fsync`, would close this.

Other limits:

- **Assumed:** the disk honours fsync, and peers run this code (no
  Byzantine or buggy proposer: C2x).
- **Duplicate `--id` values:** covered by section 4 (safe). They do cost
  extra refused rounds.
- **Wall-clock jumps forward:** they only raise ballots, which is
  harmless. Peers' NACKs raise their counters in turn.
- **The counter can run ahead of the clock.** It gains one per ballot,
  even within a millisecond. A burst of contended rounds can put it ahead
  of the clock, so even an ordinary restart within that lead can reuse
  ballots. The section 4 argument covers this case as well.
