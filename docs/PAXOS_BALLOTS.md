# Paxos ballots after a witness restart (F8)

**Status: F8 outcome A APPROVED; F9 a configuration/recovery exclusion
for the initial Profile B after the hardening of section 9 (2026-11-05,
`handoff/PR51_Review_Handoff.txt`).** Sections 1 to 8 are the
characterization (work order `handoff/PR50_Review_Handoff.txt`), kept as
written apart from status notes; section 9 is the hardening that
followed it. The characterization changed neither the protocol nor the
state file; it added test hooks behind `snc_witness --debug 1`.
Characterization evidence: `bench/results/paxos-ballots-20261105/`.
Test: `tests/test_paxos_ballots.py` (in `ctest`). Suites on the hook
commit (before section 9; results for section 9 in 9.5): release `ctest` 30/30; ASan+UBSan and TSan
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

## 6. F9 for the baseline: excluded history and guards (proposed here; built in section 9)

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

*Status note: F10, F11 and the directory-durability edge below are fixed
in section 9.*

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

- **Assumed (C2x):** the disk honours fsync; every peer runs the SNC
  Paxos implementation; proposers are not Byzantine; an ACCEPT is sent
  only after a successful phase 1. C2x shows acceptors accept conflicting
  ACCEPTs at one ballot from a proposer that skips phase 1. The proof of
  section 4 and every baseline claim rest on this assumption.
- **Duplicate `--id` values:** covered by section 4 (safe). They do cost
  extra refused rounds.
- **Wall-clock jumps forward:** they only raise ballots, which is
  harmless. Peers' NACKs raise their counters in turn.
- **The counter can run ahead of the clock.** It gains one per ballot,
  even within a millisecond. A burst of contended rounds can put it ahead
  of the clock, so even an ordinary restart within that lead can reuse
  ballots. The section 4 argument covers this case as well.

## 9. Hardening before the baseline freeze (2026-11-05)

Work order `handoff/PR51_Review_Handoff.txt`. Dispositions there:

- **F8: outcome A approved.** Cleared as a safety blocker; a
  non-blocking liveness note. No ballot persistence. C1 to C3 stay as
  permanent regression tests.
- **C2x: the non-Byzantine proposer assumption** (section 8) stays
  explicit here and in the baseline claims
  (EXPERIMENTAL_BASELINE.md section 12).
- **F9: a real consensus safety failure** under acceptor-state loss or
  rollback. For the initial Profile B campaign it is a configuration and
  recovery exclusion, after the hardening below. **For enterprise SNC it
  is NOT solved:** safe witness replacement and recovery must exist
  before any claim of consensus-member restore or replacement. Section 7
  stays the candidate design, not built.

### 9.1 What changed

| Item | Change (`tools/snc_witness.cpp`, `tools/paxos.h`) |
| --- | --- |
| **G1** explicit first start | `--new-state 1` is required to start without a state file, and refused when the file exists. Missing file, no flag: refused (exit 2), nothing created. Missing, with the flag: the file is written and made durable (file and directory fsync) before the witness serves anything. Existing, no flag: a normal start. Existing, with the flag: refused, file untouched. The witness never truncates or recreates a state file. A path that is not a regular file, or that cannot be checked, is refused |
| **G2** campaign guard | `bench/profile_check.py` (`Guard.witness`, `WitnessStateGuard`) used by `bench/lab/lab.py`. Profile B: `--new-state 1` exactly on a witness's first start in the run with its file absent; never on a restart; a restart on the same state path, which must exist. At the first start the lab writes a random run nonce beside the state (`wN.state.run`) and records the host, `--id`, normalized peers, state path, the state directory's device and inode, the nonce, and each machine's chosen version. At the end of the run it checks every witness again: the file gone, the directory replaced (other device/inode, or nonce missing), or any machine's chosen version lower than recorded marks the run **invalid** (`result.json`: `ok: false`, `invalid`, and the `witness_state` manifest). It never recreates a state file |
| **F10** legacy lines | A state file is legacy (pre-machine names) only when its `open`/`promised` lines have two fields; then every line is `ha/0`. Modern files (three fields) keep every name as written, all-digit ones included. A file mixing both, a legacy file without a whole machine, an unknown line or one that does not parse: refused (exit 2, "not starting"). A `READ` of a machine never seen no longer creates an entry, so it is never saved |
| **F11** peers | Each `--peers` entry is resolved to IPv4 `ip:port`. Refused (exit 2): an endpoint named twice (also through another host name, e.g. `localhost` and `127.0.0.1`); an `--id` whose entry is not this witness's `--port` or whose address is not on this machine; another entry that reaches this witness's own port on this machine. The existing checks (63 peers at most, `--id` in range) are kept |
| **Fatal persistence** | Every step of a save is checked: temporary file open, write, fsync, rename, directory open, directory fsync. Any failure prints `paxos: cannot save state: ...; stopping before any reply that depends on it` and exits (code 3) before the reply. Directory open and fsync failures used to be ignored. Test hook (`--debug 1` only): `DEBUG FAIL-PERSIST <step> [skip]` |

**Independent review of the slice** (read-only, before the suites)
found no blocking safety defect and these, all fixed:

- F11: two local addresses of *another* co-located witness
  (`127.0.0.1:B` and `127.0.0.2:B`) passed, and repeated ACCEPTs at one
  ballot would let one acceptor count twice in phase 2. Now every entry
  that is an address of this machine is keyed by its port alone (H4
  third case).
- F11: "local" was decided by test-binding the address, which every
  address passes under `net.ipv4.ip_nonlocal_bind=1`. Now: 127/8,
  0.0.0.0, or an address listed on an interface (`getifaddrs`).
- G1: `stat` then create could race a second start of the same witness,
  and `create()` renamed over whatever was there. Now the first state
  file is written under a unique name and *linked* into place; an
  existing file stops the start (`EEXIST`) instead of being replaced.
- A state file in `/` had an empty directory name for its fsync (always
  fatal); now `/`.
- F10: numbers in the state file must be plain decimal fields (`7x` or
  `-1` refused, H9).
- G2: the end-of-run check failing for any reason (ssh, a file it cannot
  read) now invalidates the run instead of skipping the save; the lab
  waits for each state file instead of a fixed sleep; besides chosen
  versions, each machine's (chosen, open, promised, accepted) may never
  go back, which sees a restored copy that differs only in a promise
  (C4a with the COMMIT lost).
- H2 asserts the first start serves; H6/H7's proposer case asserts the
  other witnesses accepted X, proving the failing save was the learning
  one.

**Still not detectable, excluded:** one *remote* host named in
`--peers` by two of its addresses (two NICs). A witness cannot tell from
its configuration; Profile B requires one address per witness host. A
state path that is a symlink is replaced by a regular file at the first
save (as before this slice); Profile B state paths are plain files.

No protocol message, wire format or state-file format changed. Old
witness binaries cannot read or write anything new; launch scripts must
add `--new-state 1` on first starts (all tests and `bench/` scripts do,
through `tests/witness_state.py` or the lab).

### 9.2 Tests (H1 to H14)

`tests/test_witness_hardening.py` (H1 to H9, H12), `tests/test_profile_check.py`
(H10 and the guard's G1/F11 rules); H11, H13, H14 are the existing suites.
Every witness launch in the tests (Python and C++: `test_cluster`,
`test_migrate`) and in `bench/` passes `--new-state 1` on a first start
only.
Evidence: `bench/results/witness-hardening-20261105/`.

| Test | Claim | Result |
| --- | --- | --- |
| H1 | Missing state, no flag: refused, nothing created | PASS |
| H2 | Missing, with the flag: created, durable before serving | PASS |
| H3 | Existing, with the flag: refused, file byte-identical | PASS |
| H4 | One endpoint twice, also as `localhost` and `127.0.0.1`: refused | PASS |
| H5 | `--id` on another port; on an address not of this machine; another entry reaching our port (`127.0.0.2`): refused | PASS |
| H6 | Directory open / fsync failing on PREPARE, ACCEPT, COMMIT and on a proposer's own learning save: no reply, exit 3; restarts on what is on disk | PASS (8 cases) |
| H7 | Temporary-file open / write / fsync and rename failing, same requests | PASS (16 cases) |
| H8 | A legacy file loads as `ha/0`, is saved with names, and reads the same after a restart | PASS |
| H9 | Machine `5` in a modern file stays `5`; `ha/0` stays empty across a save and restart; a stray `READ 9` creates nothing; mixed, unparsable and unknown lines refused | PASS |
| H10 | Guard: file deleted, directory recreated (even with the same inode number), older copy put back: violation; ordinary rename save: passes; lab command lines pass | PASS |
| H11 | C1 to C4 (C4b now shows G1 refusing the restart, exit 2, then reproduces F9 only with the forbidden `--new-state 1`) | PASS |
| H12 | Ordinary restart on an intact file (H2, H8, H9, and every restart in the suites) | PASS |
| H13 | READY causal order V1 to V10 (`test_ready_order`, m1 to m3 against a build of e3e0500) | PASS (9.5) |
| H14 | Witness, control, health, fail-closed regressions | PASS (9.5) |

### 9.3 Invalid histories for the initial Profile B (F9 exclusion)

Explicitly **invalid and unsupported**; if one happens, **stop the run**
and rebuild the experimental witness set, never resurrect the identity:

- restoring an individual witness's state file from a backup;
- restoring an old witness state directory;
- rolling back one witness's VM, container or filesystem snapshot;
- copying an old state file back into the same witness identity;
- copying another witness's state into this identity;
- replacing a witness's storage and starting the replacement as the old
  identity;
- using `--new-state 1` to recreate a member of an existing witness quorum.

G1 catches a deleted file or a wrong path. G2 catches, within a run, a
deleted file, a replaced directory and an older copy put back. Neither
can see a copy whose versions are at least as new (a snapshot taken and
restored with nothing chosen between), nor anything outside a campaign:
the exclusion is a rule for operators, enforced where it can be.

### 9.4 Witness backup and restore semantics

A witness's Paxos acceptor state is **not an independently restorable
snapshot.** Rolling one consensus member back while the others went on
can choose two values (C4). Until a recovering-acceptor procedure
exists (section 7), a witness's state directory, disk or VM must never be
restored backwards into the same live cluster identity.

This does **not** make future SNC application-state snapshots,
point-in-time recovery, copy-on-write clones or branches unsafe.
Application state and consensus-member metadata have different restore
semantics, and a future backup and restore design must keep them apart
(FUTURE_ARCHITECTURE.md).

**Hypervisor snapshots.** Restoring a VM or hypervisor snapshot of one
witness into a quorum that advanced after the snapshot was taken is
unsupported and unsafe. A coordinated whole-cluster disaster-recovery
procedure is a different problem, not covered here. An individual Paxos
acceptor cannot be time-travelled backwards.

### 9.5 Suites (commit 482958a, fresh builds)

| Build | Result | Sanitizer reports |
| --- | --- | --- |
| RelWithDebInfo | 31 / 31 (see below) | n/a |
| Debug, ASan + UBSan (`-fno-sanitize-recover=undefined`, leaks on) | 31 / 31 | 0 |
| Debug, TSan (`halt_on_error=1`) | 31 / 31 | 0 |

The release `ctest` run was 30 / 31: `test_ready_order` m1 and m2 failed
because `SNC_OLD_BUILD` pointed at a build of PR #39, older than the
build of e3e0500 that V10 specifies (SYNC_READINESS 7). Rerun alone with
the e3e0500 build: all 6 scenarios pass (`test_ready_order-v10.log`); the sanitizer
suites used that build. Logs: `bench/results/witness-hardening-20261105/`
(`ctest-*.log`, `hardening-json/`). An earlier release run on the
pre-QA commit failed `test_cluster` and `test_migrate`: their C++
launchers started witnesses without `--new-state 1`, fixed in 482958a.

