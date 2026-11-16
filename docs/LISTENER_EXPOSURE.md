# Listener exposure: loopback by default, AUDIT confined (AFR-S-2)

Status: **conditionally approved by Codex (2026-11-16, `docs/handoff/PR141_Listener_Exposure_Review_Conditional.md`);
conditions met at a12f4c1** (full suite 63/63, mixed-version x9 pass). Slice AFR-S-2 of
`docs/AFR_001_INVENTORY.md` (section 10), the interim mitigation of finding
**AFR-F-02** (older ID audit-D4, `docs/ARCHITECTURE_EVIDENCE_AUDIT_20261113.md`).
In the project chat this was "step 1: close the open ports first"; the
inventory's own numbering calls it AFR-S-2 (AFR-S-1 is the WAL damage slice).

Classification: **REFINE** (no invariant changes; a default and one admin
verb's argument narrow). No wire format, admin reply format or on-disk format
changes.

This slice is exposure reduction only. It does **not** add authentication,
authorization, TLS or cluster identity, and it does not touch the forged
Paxos COMMIT. Those are trust-model decisions for the guarantees spec (the
architecture review thread). Section 6 lists what stays open.

## 1. What was wrong (verified on `main` 63cf6d7 before the change)

| Listener | Opened at | Bound before | Configurable before |
|---|---|---|---|
| Client data, one port per shard | `src/net/conn_set.cpp` | `127.0.0.1`, but **`0.0.0.0` under `--control` without `--host`** | `--host` |
| Admin (`--admin-port`) | `tools/ha.h` `listen_on` | **`0.0.0.0` always** | no |
| Witness (`snc_witness --port`) | `tools/ha.h` `listen_on` | **`0.0.0.0` always** | no |
| Seed (`--seed-port`) | `src/core/engine.cpp` | **`0.0.0.0` always** | no |
| Node links (cluster `LINK_PORT + i`) | `src/core/node_link.cpp` | **`0.0.0.0` always** | no |
| Standby replication (`--standby`) | `src/core/replication.cpp` | **`0.0.0.0` by default** | `--standby-host` |

None authenticates its peer. Reproduced in this container (non-loopback
address 192.0.2.2): with a primary started as
`snc_server --data-dir D --admin-port 23001 --seed-port 23002 --port 23010`,
the request `AUDIT /tmp/snc-v1/victim`, sent to **192.0.2.2**:23001, answered
`AUDIT 0 0 0 1 0 0` and truncated the 6-byte file to 0 bytes. `AUDIT PATH`
opened PATH with `fopen(path, "w")` (`tools/snc_server.cpp`, `audit()`), so any
peer that could reach the admin port could create or truncate any file the
server's user may write, including its own WAL and superblock.

Root cause: `ha::listen_on` took no address and bound `INADDR_ANY`; the seed
and node-link listeners did the same inline; the replication receiver and the
`--control` data port defaulted to `0.0.0.0`; `AUDIT` took an unrestricted
path. Nothing ever asked the operator which network a listener belongs on.

## 2. The change

### 2.1 One bind address per process, loopback by default

- **`snc_server --host ADDR`** now sets the address of **every** listener the
  process opens: client data ports, admin port, seed port, node links, and the
  standby's replication ports. `--standby-host` still overrides the
  replication ports alone. Default `127.0.0.1`. `--host 0.0.0.0` is every
  interface, as a deliberate choice. ADDR must be an IPv4 literal (exit 2
  otherwise; clients and the control path already took literals only).
- **`snc_witness --host ADDR`** (new flag) sets the witness port's address.
  Default `127.0.0.1`.
- The `--control` fallback to `0.0.0.0` is removed (it was added in the lab
  work, `docs/REAL_MACHINES.md` finding 2; this reverses that default, not
  the need it met: section 2.2 makes the need explicit).
- Library defaults follow: `Engine::Config::listen_host` (new, seed and node
  links) and `Engine::Config::standby_host` are `127.0.0.1`;
  `repl::Receiver::Config::host` is `127.0.0.1`; `NodeLinks::Options` carries
  the address. `ha::listen_on(host, port, backlog)` now requires the host.
- Each process prints its listen address at start:
  `snc_server: listening on 127.0.0.1, loopback only (--host)`, and the same
  for `snc_witness`.

### 2.2 Refusals instead of silent unreachability

Moving to loopback must not leave a multi-machine deployment starting and
then unreachable. These cases are detectable at start and are refused with
exit 2, naming the fix:

- A cluster member (`--cluster` or `--control`) whose own map address (the
  one other nodes and clients dial; a host name is resolved, as node links
  resolve it) is not served by the bind address. The bind must be that
  exact address or `0.0.0.0`: so `127.0.0.2` in the map with the default
  `127.0.0.1` is refused, and so is `--host Y` when the map says X.
  Message: `snc_server: the placement map names this node (group G member
  M) at 10.0.0.11, but its listeners bind 127.0.0.1: nothing would answer
  there. Pass --host 10.0.0.11 (or --host 0.0.0.0 ...)`.
- A replicated witness whose own `--peers` entry is not served by its bind
  address (same rule), refused before the state file is created.
- Bound to loopback, a node whose peers on another machine dial it back:
  `--witness` on another machine with `--admin-port` (the witness polls the
  admin port); `--replica-of` on another machine with `--seed-port` (the
  standby fetches seeds); a standby whose `--seed-from` primary is on
  another machine (that primary dials it; not checked when `--standby-host`
  sets the replication address); `--rejoin` with the peer on another
  machine.
- Bound to loopback, a witness watching (`--group`, `--primary`,
  `--standby`) nodes on another machine (they dial it); and a replicated
  witness whose own `--peers` entry is loopback while another entry is on
  another machine.

A peer "on another machine" is one whose host resolves to an address that
is none of this machine's (127/8, 0.0.0.0, or an interface address from
`getifaddrs`): a peer named by this machine's own LAN address is local and
can dial back over loopback. A name that does not resolve is not counted. Still not detectable: a plain
standby (`--standby`, no `--seed-from`, no cluster map) whose primary is
on another machine. Pass `--host` (or `--standby-host`) on such a standby.

### 2.3 AUDIT writes only inside an operator-chosen directory

- `AUDIT` with no argument is unchanged.
- `AUDIT NAME` writes the same `ID BALANCE` lines as before, to the file
  NAME inside the directory given at start with **`--audit-dir DIR`** (new
  flag). Reply format unchanged.
- NAME must be 1 to 128 characters of `A-Z a-z 0-9 . _ -` and must not start
  with `.`; so no `/`, no `..`, no absolute path. Anything else is answered
  `NO ...` and nothing is opened.
- The balances are written to a new file created with `O_EXCL|O_NOFOLLOW`,
  mode 0600, under a temporary name AUDIT cannot be given (leading `.`), in
  a directory descriptor opened at start, then `renameat` over NAME. An
  existing NAME is therefore never opened: a hard link named NAME is
  replaced, not written through (QA 1 found that the first version,
  `O_TRUNC` on NAME, truncated a hard link's target). An existing NAME that
  is a symbolic link, a directory, a FIFO or anything but a regular file is
  refused and left alone.
- Without `--audit-dir`, `AUDIT NAME` is refused (`NO AUDIT NAME needs
  --audit-dir on this node`). An old caller that still sends a path gets
  `NO`, which the harnesses already treat as failure, not as an empty audit.
- `--audit-dir` that does not exist, that is the data directory or inside
  it (compared by device and inode, walking up from the open directory, so a
  symbolic link or bind mount cannot hide it), or that holds
  `--links-generation-file`, is refused at start (exit 2): a name there
  could replace the node's own files. Another node's data directory on the
  same machine is not known to this node: keep the audit directory apart.

The inventory offered "remove or confine" the path argument. Removing it
outright would break the lab's and `test_baseline_move`'s end-of-run balance
check (`snc_load check` reads these files), which is the strongest
correctness check the multi-machine runs have. Confining it to a directory
the operator names keeps that check and closes the arbitrary-file write.

## 3. Callers changed

| Caller | Change |
|---|---|
| `bench/lab/lab.py` `node_args` | new keyword args `host`, `audit_dir`; `start_node` passes `host=<host addr>` and `audit_dir=<run dir>/audit` (its own directory, made before start: the run dir holds witness state, QA 2) |
| `bench/lab/lab.py` `witness_args` | new keyword arg `host`; `up()` passes the witness host's address |
| `bench/lab/lab.py` `check()` | `AUDIT bal.K` instead of `AUDIT <run dir>/bal.K`; fetched from `<run dir>/audit/bal.K` |
| `bench/lab/lab.py` `p1_p2` | the P2 standby gets `--host <standby addr>` (its primary is on another host) |
| `tests/test_baseline_move.py` | servers get `--audit-dir <work>/audit`; `AUDIT bal.K` |
| `bench/profile_check.py` | `--audit-dir` is structural for `snc_server`, `--host` for `snc_witness` |

Every other launcher in `tests/` and `bench/` (pilot, S6, CP-1A, run_*.py,
run_replication.sh) is single-machine on `127.0.0.1` and needs nothing.

**R640 and other real-machine runs.** The CP-1A/S6 pilots run server and
client on the R640 over loopback: unaffected. A lab run across machines
picks the new flags up from `lab.py`. Any hand-written launch that relied
on the old all-interfaces default must now pass `--host` (a cluster member
or replicated witness is refused with that instruction; a plain standby is
not, see 2.2).

## 4. Verification

`tests/test_listen_exposure.py` (fast tier, about 4 s), real processes,
deterministic: it reads the kernel's listening-socket table
(`/proc/net/tcp`) for the ports it chose, and, where the machine has a
non-loopback IPv4 address, also connects to it.

| Case | What it checks | On `main` 63cf6d7 |
|---|---|---|
| L1 defaults | data, admin, seed, standby replication, cluster node-link and witness ports listen on `127.0.0.1` only; each is refused at the non-loopback address | FAIL (admin and seed on `0.0.0.0`) |
| L2 explicit | `--host 0.0.0.0` binds every interface (server and witness); `--standby-host 127.0.0.1` still overrides replication | FAIL only on the new start-up line |
| L3 AUDIT | `AUDIT /path` leaves the named file intact; `AUDIT NAME` without `--audit-dir` refused; with it the file lands in the directory; `/`, `..`, leading `.`, 129 characters and a symbolic link are refused and the link target is untouched | FAIL (the file was truncated) |
| L4 refusals | non-loopback cluster address without `--host`: exit 2 naming `--host`; with `--host 0.0.0.0` it starts; `--host localhost` refused; `--audit-dir` equal to or inside `--data-dir` refused; missing `--audit-dir` refused; witness with a non-loopback local `--peers` entry and no `--host`: exit 2, no state file | FAIL (the member started) |
| L5 `--control` | a node's admin, client and link ports on loopback without `--host`; a node the placement map names at 192.0.2.77 is refused without `--host` | FAIL (all three on `0.0.0.0`) |

Full suite results, and the three independent QA reviews, are in section 7.

## 5. Compatibility

- Wire formats (client, admin replies, witness/Paxos, replication, seed,
  node link): unchanged. On-disk formats: unchanged.
- Admin protocol: `AUDIT` with an argument now means a file name in
  `--audit-dir`, not a path. This is the one protocol-visible narrowing.
- Defaults: every listener moves from all interfaces to loopback. That is
  the purpose of the slice.
- Mixed versions: an old `snc_server` or `snc_witness` in a cluster keeps
  binding all interfaces; a new one beside it binds what it is told.
  Nothing in the protocols depends on the bind address.

## 6. What this slice leaves open (AFR-F-02 items, for the guarantees spec)

**Authentication, authorization and cluster identity remain explicitly open
for decision AFR-D-07** (`docs/SNC_GUARANTEES.md`; Codex's PR #141 review,
condition 4). This slice does not design them and must not be read as
covering them.

Loopback is not authentication. Any local user or process on the same
machine still reaches every port, and any machine reaches them once an
operator passes `--host`. Still open, unchanged by this slice:

1. **No authentication or authorization on any listener**, no TLS, no
   cluster identity in any protocol (AFR-F-02 overall; audit-D4).
2. **`PROMOTE` has no term or identity guard** (AFR-F-02 item 3).
3. **The witness accepts a forged Paxos `COMMIT`** (and a PREPARE's base
   value) with a higher version and persists it as chosen; other witnesses
   may adopt it (item 4).
4. **The standby's replication receiver** drops its live primary when any
   new connection is accepted, before a Hello is read, and a peer past the
   Hello can extend a standby's read lease (item 5).
5. **The only identity checks use values `HEALTH` publishes**; the seed port
   serves a full checkpoint to an all-zero uuid; `MOVE HAVE` and
   `MOVE EXPORT` return object payloads (item 6).
6. The client data port serves any peer that reaches it (by design today).
7. `AUDIT NAME` may still replace files inside `--audit-dir`, by design;
   the directory should hold nothing else.
8. `tools/migrate.h` `AdminServer` (test-only, used by `test_migrate`) binds
   loopback and takes no `--host`; nothing else constructs it.
9. A library caller that gives `NodeLinks` an address that is not an IPv4
   literal sees the link listener retry and log EINVAL, not a start error
   (`snc_server` validates `--host` first, so it cannot reach this).

Against the proposed guarantees (`docs/SNC_GUARANTEES.md`, PR #139): this
slice is the interim step of ruling AFR-R-21. It meets AFR-G-42 ("no
management operation writes to a caller-chosen file-system path") for
`AUDIT` as far as the caller is concerned: the caller picks only a name in
a directory the operator chose. AFR-G-40, 41, 43, 44, 63 and 64 are
untouched.

The inventory's required verification for these (forged-COMMIT and
forged-Hello tests that must be refused once authentication exists) belongs
with the trust-model decision, not here.

## 7. Evidence and QA

**Full suite** (`ctest -j1`, RelWithDebInfo, this container, final C++ at
d2fc313): 62 of 63 passed. The one failure was real and caused by this
slice: `test_xnode_proc` x6/x7/x7b check balances through
`test_baseline_move.settle_and_check`, whose cluster did not pass
`--audit-dir`, so `AUDIT bal.K` was refused and the checker reported "64
accounts served by 0 nodes" (loud, not silent). Fixed in the harness:
`test_xnode_proc` nodes get `--audit-dir`, a node of an older build (x9,
run by name with `--old`) is asked with a path as before, and
`settle_and_check` now fails at once on a refused AUDIT instead of reading
it as an empty node. Rerun after the fix: `test_xnode_proc`,
`test_baseline_move`, `test_listen_exposure` pass. x9 (needs an old build)
was not run. `test_xnode` X16 (known flake) passed in this run.

**Regression test:** `test_listen_exposure` passes on the final build (5 of
5 cases, repeated runs, no flakes) and fails on `main` 63cf6d7 in L1, L3, L4
and L5 (L2 only on the new start-up line).

**Three independent QA reviews** (separate agents, two rounds each for QA 1
and QA 2). No blockers in any round. Findings and resolution:

| # | From | Finding | Resolution |
|---|---|---|---|
| 1 | QA 1, QA 3 | start check refused a map that names the node by a host name (and advised an impossible `--host localhost`) | names resolved before the check; the fix names the resolved IPv4 |
| 2 | QA 1 | **a hard link named NAME let AUDIT truncate a file outside `--audit-dir`** (`O_TRUNC` before the type check) | new file with `O_EXCL`, mode 0600, renamed over NAME; existing non-regular NAME refused; test L3 covers hard link, symbolic link, FIFO, directory |
| 3 | QA 1 | `--audit-dir` could hold `--links-generation-file` (also via a symbolic link) | refused at start, file resolved first |
| 4 | QA 1 | pre-created file in a shared audit dir; predictable temp names | covered by 2; temp names random and retried |
| 5 | QA 1 | data-dir comparison by path string (bind mount, `//` prefix) | device/inode walk from the open directory; QA 1 confirmed with a real bind mount |
| 6 | QA 1, QA 2 | refusal rules differed between server and witness; 127.0.0.2 and `--host Y` vs map X passed | one rule in both: the bind must be the advertised address or 0.0.0.0 |
| 7 | QA 2 | other silent-unreachable HA cases (witness watching remote nodes, node with remote `--witness`, etc.) | refused at start (section 2.2) |
| 8 | QA 1 (round 2) | the new refusals treated this machine's own LAN address as remote | local interface addresses count as this machine |
| 9 | QA 1 (round 2) | witness with loopback self entry and remote peers | refused |
| 10 | QA 2 | lab used the run directory (holds witness state) as `--audit-dir` | `run_dir/audit`; QA 2 reran simulated multi-host lab p3 (stop fault) and p5, profile B, check OK |
| 11 | QA 3 | weak assertions (`refused()` accepted any error; message-free rc checks) | tightened |
| 12 | QA 1 | data-port bind error did not name the address | `bind HOST:PORT` in the message |

Left as notes (section 6): `AdminServer` in `tools/migrate.h` is test-only
and loopback; `NodeLinks` retries a non-literal address instead of failing.
