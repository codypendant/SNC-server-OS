# Real-machine test plan

Status (2026-10-13): the harness is built and passes on loopback
(reference numbers below). No physical hosts yet; findings go in the last
section and in the `AGENTS.md` handoff log as they come in.

Every distributed test so far ran on one 4-vCPU VM over loopback. Loopback
has near-zero RTT, never loses or delays a packet, and turns every crash
into an immediate connection reset, because a killed process's kernel is
still alive to send one. Real hosts differ in all three. This plan runs
the existing replication, failover, cluster and migration behaviour across
physical hosts before any more distributed-control features are added
(Codex review of PR #24, 2026-10-13).

## Hardware

Minimum, 3 hosts (each runs one witness):

| Host | Runs |
| --- | --- |
| A | witness 0, group 0 primary, group 1 standby |
| B | witness 1, group 1 primary, group 0 standby |
| C | witness 2, load clients, the move driver (`snc_ctl`) |

A primary and its standby never share a host. Preferred is 5 or more: a
third group on its own pair of hosts and the clients on a host that runs no
node, so client CPU does not mix into node numbers.

Each host needs: Linux 6.x, a local NVMe or SSD for the data dirs (record
its fsync latency), the same NIC speed on one switch (record it), root or
`CAP_NET_ADMIN` for the fault tools (`tc netem`, `iptables`, `ip link`),
SSH between the hosts with keys, and clocks running NTP (SNC needs a
bounded drift rate, not synchronised clocks; record the offsets anyway). A
second site or a `netem` delay gives the high-RTT runs.

Access: this project's cloud sessions cannot reach a private lab. cody
chose (2026-10-13) to run a Claude Code session on host C, which drives the
other hosts over SSH.

### Setting up a host

Ubuntu Server 24.04 LTS (kernel 6.8; SNC needs 6.x for io_uring), or
Debian 13. Then:

```sh
sudo apt install build-essential cmake ninja-build git liburing-dev \
                 iperf3 fio iproute2 iptables
```

On host C only: clone the repository and build
(`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo &&
cmake --build build`), give it SSH keys to root on A and B, copy
`bench/lab/inventory.example.toml` to `bench/lab/inventory.toml` with the
real addresses, NIC names and cores, then `bench/lab/lab.py
bench/lab/inventory.toml deploy`. The other hosts need no build: deploy
copies the binaries. Same CPU architecture on every host.

## Harness

Built (2026-10-13):

- `bench/lab/lab.py INVENTORY SCENARIO`: starts the witnesses, the
  placement (`snc_ctl init`) and every group's standby and primary on the
  inventory's hosts (bash locally, SSH otherwise); applies and heals
  faults; polls every node's `STATUS`; and writes
  `bench/results/lab-DATE/SCENARIO-TIME/result.json` with the measurements,
  a timeline of events, the load's summary and every log. Scenarios `p0`
  to `p5` and `soak` are the phases below. `deploy` copies the binaries.
- `bench/lab/loopback.toml`: every role on this machine. Same code, the
  reference numbers. `bench/lab/inventory.example.toml`: three hosts.
- `snc_load` (`bench/snc_load.cpp`): `create` makes accounts spread over
  the groups, plus a full slot to move; `run` sends random transfers
  through a `ClusterClient` with sessions (follows failovers and moves),
  prints throughput and latency percentiles per interval, and keeps each
  account's net acknowledged change and every in-doubt transfer; `check`
  takes the nodes' balance files and applies `test_migrate`'s checks:
  every account served exactly once, and every balance equal to its start
  plus the acknowledged transfers plus some subset of the in-doubt ones.
- `snc_server` admin `AUDIT [PATH]`: accounts served now, their total,
  unsettled transfer entries, epoch, held and misrouted follow-ups; with
  PATH it writes the balances there.
- `snc_ctl move --stop-after STEP` (a driver that dies at a step, for
  crash runs) and `--chunk N` (objects per copy request ahead of the
  seal). The driver's log lines now carry elapsed time.
- A `Probe` in `lab.py` asks every node `MOVE INFO SLOT` every 20 ms
  during a move: two groups serving is a failure, no group serving is the
  client-visible window.

Faults: `kill` and `stop` everywhere; `partition`, `oneway`, `link` and
`delay` need real hosts (root, `iface` in the inventory). Inventories use
IPv4 literals: clients and the control path do not resolve names.

Not automated: restarting a failed primary as its group's new standby
(`--rejoin` is not exercised inside a cluster yet), so `soak` fails each
group over at most once per member.

## Phases

Each phase records the loopback number next to the real one.

**P0. Baseline** (`p0`). RTT p50/p99 between every pair (`ping -c 1000 -i 0.01`),
`iperf3` bandwidth, `fio` fsync latency on each data disk.

**P1. One node, remote clients** (`p1`). `net_bench` from C against one node on
A, in memory and durable, depth 1 to 512. Expect lower throughput at low
depth by RTT/service time, and the loopback number back at high depth if
the NIC is not the limit. Checks the routed front end and `TCP_NODELAY`
over a real NIC.

**P2. Sync replication across hosts** (`p2`). Group 0 (A primary, B standby).
Every durable ack now waits a network round trip on top of the fsync:
measure throughput and p50/p99 against no standby (0.54 to 0.60x on
loopback) and against `group_commit_us`. Seed a standby with 1M accounts
(about 5 s on loopback), catch-up rate with and without
`--catchup-mib-s`, the degrade at `replica_timeout_ms` (2 s) when the
standby's link is dropped, and standby reads under leases.

**P3. Failover** (`p3 --fault kill|stop|partition|oneway|link`). Witnesses on A, B and C. For each fault on group 0's
primary (`SIGKILL`, `SIGSTOP`, power-off, link down, cut from its standby
only, cut from the witnesses only, cut in one direction): time to promote
(witness `failover_ms` 1000, `poll_ms` 100), time until clients resume,
acknowledged writes lost (must be 0), and time until a live old primary
withdraws. The known gap (a primary cut off from its standby and every
witness but reachable by other groups blocks and keeps its links) gets a
dedicated case.

**P4. Cluster** (`p4 --fault partition|delay`). Two or three groups, random cross-group transfers under
load. Settle latency (each follow-up crosses a real link), throughput,
backlog. Then cut one node link and restore it: time to reconnect and to
settle the backlog; a reconnect re-runs resume, which walks every object,
so measure it at 1M objects. Repeat with `netem` 1, 5 and 20 ms.

**P5. Migration** (`p5 [--slot-size N] [--chunk N] [--crash driver|source|dest --at STEP]`). `snc_ctl move` under load with slots of 1K, 10K, 100K
and 1M objects (`create_near` fills one slot). Per step: time, bytes,
copy throughput, and the window in which clients see `E_MIGRATING`
(sealed until activated). Expect the final copy to fail once a slot's
copy takes longer than the 15 s admin-line timeout or grows past what one
line carries: that size is the evidence for the chunked stream
(`docs/MIGRATION.md`, "Next"). Aborts at each step. Then a subset of the
`test_migrate` crash matrix on real hosts: driver, source primary and
destination primary each killed (and powered off) after each step, with
the same checks.

**P6. Soak** (`soak --minutes M`). Several hours of mixed load with random faults from P3 and
P4 and moves back and forth, checked continuously and at the end.

## Assumptions that may not hold outside loopback

Predicted from the code; each phase above confirms or clears them.

1. **A dead host looks like a hung connection, not a reset.** On loopback
   a killed peer's kernel resets the connection at once. A powered-off
   host or a dropped link sends nothing. Replication notices through its
   ack timeout. Node links set no `SO_KEEPALIVE` or `TCP_USER_TIMEOUT`
   and have no acks, so an established link to a dead host may keep
   accepting writes into the socket buffer until the kernel's retransmit
   limit (about 15 minutes by default) and only then reconnect and run
   resume. Follow-ups are recoverable (invariant 15), so this costs
   settle time, not safety. Covered by P3 and P4.
2. **Timeouts were tuned on loopback.** Node link connect 1 s and retry
   100 ms (`src/core/node_link.cpp`), witness asks 2 to 5 s
   (`tools/control.h`, `tools/migrate.h`), the move's final copy 15 s per
   admin line, witness failover 1 s. With real RTT, jitter and loss some
   may be too short (false failovers) or too long.
3. **Sync replication cost changes shape.** On loopback it is fsync plus
   CPU contention on 4 vCPUs; across hosts it is fsync plus RTT, with the
   CPU contention gone. The 0.54 to 0.60x ratio will move in either
   direction.
4. **Whole-slot copies in one admin line** work only for test-sized slots.
5. **Resume walks every object** on every reconnect; harmless on loopback
   where reconnects are rare and fast, possibly costly with flapping links.
6. **Leases rely on bounded drift.** Standby read leases and the
   primary's wait-out use each host's monotonic clock; drift between real
   hosts is small but nonzero.
7. **Single-VM numbers mix client and server CPU.** Separate hosts remove
   that, and a multi-socket host would make NUMA measurable for the first
   time.

## Loopback reference (2026-10-13)

The 4-vCPU VM, everything on it at once: three witnesses, two groups of a
durable sync primary and standby (2 shards each), the load (2 threads,
depth 16) and the probe. Throughput here is CPU-bound by sharing four
cores among all of that; compare shapes, not absolute numbers. Results in
`bench/results/lab-20261013/`.

| Run | Result |
| --- | --- |
| P3 `kill` group 0's primary | promoted 1.00 s after the kill (`failover_ms` 1000); no transfer acknowledged for at most one 0.5 s report interval, max latency 0.98 s; 13.8K transfers/s; check OK (20,000 accounts, none in doubt) |
| P3 `stop` (hung primary) | promoted 1.06 s after; after `SIGCONT` the old primary withdrew (`role=f`) in 0.12 s; no transfer acknowledged for 2.0 s (the hung primary holds requests until the client's 3 s request timeout); check OK |
| P5 move, 2,012-object slot, `--chunk 8` | 6.05 s there and 6.12 s back, of which 5.7 s is the copy ahead of the seal; seal to activation 0.25 s; probe: never two serving groups, 0.18 s with none serving; every node at epoch 3; check OK |
| P5 move, same, `--chunk 256` | 0.56 s there and 0.97 s back; same window and checks |

## Findings

From building the harness, on loopback:

1. **The copy ahead of the seal is round-trip bound.** It stages 8
   objects per request and waits for each stage to be durable at the
   destination (fsync and its standby's ack), so a slot of N objects costs
   N/8 durable round trips: 5.7 of 6.0 s for 2,012 objects. With 256 per
   request the whole move takes 0.56 s. Across hosts each round trip also
   pays the RTT. This does not touch the window clients see (seal to
   activation, 0.25 s), only how long a move runs. Evidence for the
   chunked stream; `--chunk` lets P5 measure it meanwhile.
2. **A node started with `--control` listened for clients on loopback
   only**, although the map advertises its host to other hosts' clients.
   Fixed: it now listens on every interface unless `--host` is given.
3. A promoted standby whose group has no other member reports `role=n`
   (no replication), not `p`; the lab accepts either.

Predicted, to confirm on hosts: the assumptions above.
