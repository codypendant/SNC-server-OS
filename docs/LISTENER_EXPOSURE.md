# Listener exposure: loopback by default, AUDIT confined (AFR-S-2)

Status: **implemented, for review** (cody and Codex). Slice AFR-S-2 of
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

Moving to loopback must not leave a multi-machine cluster starting and then
unreachable. Two cases are detectable at start and are refused with exit 2,
naming the fix:

- A cluster member (`--cluster` or `--control`) whose own map address (the
  one other nodes and clients dial) is not loopback, while its bind address
  is loopback: `snc_server: the placement map names this node (group G
  member M) at 10.0.0.11, but its listeners bind 127.0.0.1: other machines
  could not reach it. Pass --host 10.0.0.11 (or --host 0.0.0.0 ...)`.
- A replicated witness whose own `--peers` entry is not loopback, with no
  `--host`: the same message for `snc_witness`. It is refused before the
  state file is created.

Not detectable at start, and therefore documented here and in the start-up
line: a standby (`--standby`, no cluster map) whose primary is on another
machine. Its primary dials it and fails; the standby cannot know the
primary's address. Pass `--host` (or `--standby-host`) on such a standby.

### 2.3 AUDIT writes only inside an operator-chosen directory

- `AUDIT` with no argument is unchanged.
- `AUDIT NAME` writes the same `ID BALANCE` lines as before, to the file
  NAME inside the directory given at start with **`--audit-dir DIR`** (new
  flag). Reply format unchanged.
- NAME must be 1 to 128 characters of `A-Z a-z 0-9 . _ -` and must not start
  with `.`; so no `/`, no `..`, no absolute path. Anything else is answered
  `NO ...` and nothing is opened.
- The file is opened with `openat(dirfd, NAME, O_WRONLY|O_CREAT|O_TRUNC|
  O_NOFOLLOW|O_NONBLOCK)` against a directory descriptor opened at start: a
  symbolic link named NAME is refused (not followed), and anything that is
  not a regular file is refused after open.
- Without `--audit-dir`, `AUDIT NAME` is refused (`NO AUDIT NAME needs
  --audit-dir on this node`). An old caller that still sends a path gets
  `NO`, which the harnesses already treat as failure, not as an empty audit.
- `--audit-dir` that does not exist, or that is the data directory or inside
  it, is refused at start (exit 2): a name there could replace the node's
  own files.

The inventory offered "remove or confine" the path argument. Removing it
outright would break the lab's and `test_baseline_move`'s end-of-run balance
check (`snc_load check` reads these files), which is the strongest
correctness check the multi-machine runs have. Confining it to a directory
the operator names keeps that check and closes the arbitrary-file write.

## 3. Callers changed

| Caller | Change |
|---|---|
| `bench/lab/lab.py` `node_args` | new keyword args `host`, `audit_dir`; `start_node` passes `host=<host addr>` and `audit_dir=<run dir>` |
| `bench/lab/lab.py` `witness_args` | new keyword arg `host`; `up()` passes the witness host's address |
| `bench/lab/lab.py` `check()` | `AUDIT bal.K` instead of `AUDIT <run dir>/bal.K` (same resulting path) |
| `bench/lab/lab.py` `p1_p2` | the P2 standby gets `--host <standby addr>` (its primary is on another host) |
| `tests/test_baseline_move.py` | servers get `--audit-dir <work>`; `AUDIT bal.K` |
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
7. `AUDIT NAME` may still overwrite files inside `--audit-dir`, by design;
   the directory should hold nothing else.

The inventory's required verification for these (forged-COMMIT and
forged-Hello tests that must be refused once authentication exists) belongs
with the trust-model decision, not here.

## 7. Evidence and QA

(Filled in with the final PR: full-suite run, three independent QA reviews,
their findings and how each was resolved.)
