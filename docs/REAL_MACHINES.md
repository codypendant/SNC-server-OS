# Real-machine test plan

Status: plan (2026-10-13). Nothing here has been run yet; findings go in
the last section and in the `AGENTS.md` handoff log as they come in.

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

Access: this project's cloud sessions cannot reach a private lab. The runs
are driven from host C, either by a Claude Code session on that host or by
cody running the harness and sharing `results/`.

## Harness

Needed before phase 1 (to be built; no machines are needed to build it):

- `bench/lab/inventory.toml`: hosts, SSH targets, data dirs, CPU lists,
  and the port plan (below 32768, per the existing tests).
- `bench/lab/lab.py`: deploys one build to every host, starts and stops
  roles, applies and clears faults, collects logs and admin `STATUS`
  snapshots into `bench/results/lab-<date>/`. The same scenarios run
  against `localhost` for a loopback reference with identical code.
- A load client that records every acknowledged transfer (the in-process
  tests keep this log; `net_bench` does not), so the lab can apply the same
  checks as `test_migrate`: every acknowledged transfer in the final
  balances, money conserved, all settled, never two serving groups.
- Hostnames: the client, the HA/admin path and `conn_set` parse IPv4
  literals only (`inet_pton`); node links, replication and seeding resolve
  names. Inventories use IPs until that is fixed.

Faults it can apply: `SIGKILL`; `SIGSTOP`/`SIGCONT` (hung process,
connections stay up); power-off or reboot; `ip link set down` on the
data NIC; `iptables` drops between chosen host pairs, in one or both
directions; `tc netem` delay, jitter and loss on a host pair.

## Phases

Each phase records the loopback number next to the real one.

**P0. Baseline.** RTT p50/p99 between every pair (`ping -c 1000 -i 0.01`),
`iperf3` bandwidth, `fio` fsync latency on each data disk.

**P1. One node, remote clients.** `net_bench` from C against one node on
A, in memory and durable, depth 1 to 512. Expect lower throughput at low
depth by RTT/service time, and the loopback number back at high depth if
the NIC is not the limit. Checks the routed front end and `TCP_NODELAY`
over a real NIC.

**P2. Sync replication across hosts.** Group 0 (A primary, B standby).
Every durable ack now waits a network round trip on top of the fsync:
measure throughput and p50/p99 against no standby (0.54 to 0.60x on
loopback) and against `group_commit_us`. Seed a standby with 1M accounts
(about 5 s on loopback), catch-up rate with and without
`--catchup-mib-s`, the degrade at `replica_timeout_ms` (2 s) when the
standby's link is dropped, and standby reads under leases.

**P3. Failover.** Witnesses on A, B and C. For each fault on group 0's
primary (`SIGKILL`, `SIGSTOP`, power-off, link down, cut from its standby
only, cut from the witnesses only, cut in one direction): time to promote
(witness `failover_ms` 1000, `poll_ms` 100), time until clients resume,
acknowledged writes lost (must be 0), and time until a live old primary
withdraws. The known gap (a primary cut off from its standby and every
witness but reachable by other groups blocks and keeps its links) gets a
dedicated case.

**P4. Cluster.** Two or three groups, random cross-group transfers under
load. Settle latency (each follow-up crosses a real link), throughput,
backlog. Then cut one node link and restore it: time to reconnect and to
settle the backlog; a reconnect re-runs resume, which walks every object,
so measure it at 1M objects. Repeat with `netem` 1, 5 and 20 ms.

**P5. Migration.** `snc_ctl move` under load with slots of 1K, 10K, 100K
and 1M objects (`create_near` fills one slot). Per step: time, bytes,
copy throughput, and the window in which clients see `E_MIGRATING`
(sealed until activated). Expect the final copy to fail once a slot's
copy takes longer than the 15 s admin-line timeout or grows past what one
line carries: that size is the evidence for the chunked stream
(`docs/MIGRATION.md`, "Next"). Aborts at each step. Then a subset of the
`test_migrate` crash matrix on real hosts: driver, source primary and
destination primary each killed (and powered off) after each step, with
the same checks.

**P6. Soak.** Several hours of mixed load with random faults from P3 and
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

## Findings

None yet.
