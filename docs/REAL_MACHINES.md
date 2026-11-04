# Real-machine test plan

Status (2026-11-04): first run on physical hosts — three Vultr VPSs in
Dallas (`bench/lab/vultr.toml`). P0-P5 and a 20-minute soak ran; results
and the assumptions they confirmed or changed are in "Vultr run
(2026-11-04)" below, next to the loopback reference, and in the `AGENTS.md`
handoff log. The intermittent balance-check failure under the `partition`
fault has a cause and a fix; 24 of 24 rounds on fresh hosts then passed,
which is consistent with the fix but does not prove it alone (see that
section).

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
sudo apt install build-essential cmake ninja-build git liburing-dev libmimalloc-dev libxxhash-dev \
                 iperf3 fio iproute2 iptables
```

Three fresh Ubuntu 24.04 VPSs with public IPs (the Vultr layout): run
`bench/lab/vultr_setup.sh A_IP B_IP C_IP` as root on a, then b, then c. It
installs the packages, opens tcp 21000-21600 between the hosts in ufw, and
on c clones, builds, installs c's SSH key on a and b, writes the addresses
into `bench/lab/vultr.toml` and deploys. Otherwise, by hand:

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

## Vultr run (2026-11-04)

Three Vultr VPSs in Dallas (`bench/lab/vultr.toml`), each Ubuntu 24.04,
kernel 6.8, **2 vCPU (Skylake), 3.8 GB RAM, 75 GB disk, one NIC
`enp1s0`**. c = 45.32.205.25 (clients, move driver, witness 2); a =
45.32.201.96 (group 0 primary, group 1 standby, witness 0); b =
45.76.56.244 (group 1 primary, group 0 standby, witness 1). **Public IPs
were used** — Vultr private networking was not enabled, so each host has
only `enp1s0` with a public address. ufw is on by default (SSH only); tcp
21000-21600 was opened between the three hosts, without which every node
link and witness message is silently dropped. A primary and its standby
must not share a host, so both groups span a and b; each of a and b runs
two `snc_server` processes (4 pinned shard threads) on 2 cores plus a
witness, so absolute throughput is low — compare shapes. Results in
`bench/results/lab-20261104/`.

Three loopback-only assumptions in the harness/tests were fixed here:
`test_runtime.cpp`'s 60 s delivery deadline assumed >= 4 cores (4 shards on
2 cores take ~102 s; it now scales by `ceil(shards/cpus)`); P0's `iperf3`
used its default port 5201, outside the firewall-opened range, and hung
~130 s (now `base_port+500` with `--connect-timeout`); and `check()` now
preserves its inputs (`.delta`, `.unknown`, `ids`) on a failed check for
post-mortem.

| Run | Result |
| --- | --- |
| P0 baseline | RTT 0.34-0.39 ms avg between every pair (one datacentre, sub-ms); iperf3 7.7 Gbit/s; fsync p50/p99 a 395/586 us, c 428/864 us, b 774/1171 us (b's disk ~2x slower) |
| P1 one node, clients on c | memory d1/d16/d128 = 10.0K/125.7K/658K ops/s; durable = 3.9K/36.9K/255K; 0 errors. Depth 1 is RTT/service-time bound, high depth recovers |
| P2 sync replication | durable+sync d1/d16/d128 = 3.85K/38.8K/249.5K, **0.98/1.05/0.98x** of P1 non-sync durable (loopback was 0.54-0.60x): the standby's own cores remove the loopback CPU contention and the 0.33 ms RTT overlaps the fsync |
| P3 `kill` / `stop` | promote 1.45 s / 1.63 s; `stop` old primary withdrew 0.16 s after SIGCONT; client gap 0.5 s / 2.0 s; check OK |
| P3 `partition` / `oneway` | promote 7.7-8.5 s / 7.9 s — a silent cut sends no RST, so failover waits out timeouts (~5x the process-fault case); client gap 3.5 s / 6.5 s; check OK except an intermittent partition failure (see Findings) |
| P4 `partition` (10 s cut) | cross-group follow-up backlog grows ~linearly to ~26K unsettled, drains 8.1 s after heal; check OK. On 3 hosts a host-level cut also fails over both groups (6 failovers); isolating one node link needs the 5-host layout |
| P4 `delay` (netem 1/5/20 ms) | no failover, 0 in-doubt; throughput 9.7K/7.9K/7.7K ops/s; 20 ms is well inside `failover_ms` 1000 |
| P5 move 1K/10K/100K, `--chunk 8` | 4.8 s / 43.5 s / **FAILED**; move time is N/chunk durable round trips + RTT each; E_MIGRATING window stays 0.1-0.2 s regardless |
| P5 move 1K/10K/100K, `--chunk 256` | 0.58 s / 2.9 s / 11.3 s; all DONE, check OK |
| P5 100K / `--chunk 8` | after ~197 s of copy-ahead the driver returned `STUCK no control member answered` and left the slot sealed, not activated (31,295 accounts served by 0 nodes); a retry was refused `BUSY`. Evidence for the chunked stream (assumption #4) — but the failure is a control-plane timeout under the long copy, not the predicted clean 15 s admin-line error |
| P5 crash matrix | `--crash source`/`dest` `--at sealed`/`committed`, all four: standby promoted, move DONE, never two serving groups, money conserved; E_MIGRATING window widens to ~0.6-0.7 s across a crash |
| P6 soak (20 min) | 30 rounds of mixed load with random kill/stop and moves back and forth; every round's load clean (~17-19K ops/s), final check OK (101,000 accounts each served once, 77/77 in doubt resolved) |
| P3 `partition` x24, with the PR #28 fix (`74209a8`, fresh hosts) | **24 of 24 passed.** Promote 7.66-8.41 s (median 7.71); client gap 3.5-7.0 s; every old primary reported fenced; 4,393,822 acknowledged transfers; 14-26 in doubt per round, all resolved. See below |

### Partition re-run with the fix (2026-11-04)

Three new VPSs of the same kind, same layout (a = 149.28.249.82, b =
107.191.45.215, c = 155.138.246.227), 04:08-04:26 UTC. Results:
`bench/results/lab-20261104/p3-040835` to `p3-042527`.

**What this shows.** No round reproduced the double-apply. Every round's
check found 100,000 accounts each served once and balances equal to the
acknowledged transfers plus the in-doubt ones. Every round also ran under
overload: clients got 489-4,247 `E_OVERLOADED` per round.

**What it does not show.**

- *It is weak evidence alone.* The old failure was seen about once in 24
  rounds. At that rate, 0 failures in 24 still has about a 36% chance
  without any fix, and 0 of 24 only bounds the failure rate below about 12%
  (95%). The case for the fix rests on the identified cause and on
  `test_resend_overloaded`, which fails without it.
- *The trigger's frequency was not measured.* `snc_load` counts every
  `E_OVERLOADED`, not only refused resends, so these rounds do not show how
  often a duplicate reached the overload check. The one failing round had
  10,942 refusals, above this run's maximum of 4,247, so the heaviest
  overload seen before was not matched here.
- *The rounds are repeats.* `p3` ignores `--seed` (only `soak` reads it), so
  they differ by timing alone.
- *A pass covers the balance check only.* `p3` logs an old primary that does
  not withdraw without failing the round; none of these logged it.

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

### From the Vultr run (2026-11-04)

How the predicted assumptions turned out:

1. **A dead host looks like a hung connection, not a reset — confirmed,
   and it dominates failover time.** `kill`/`stop` promote in ~1.5 s (the
   process's kernel still sends a RST); `partition`/`oneway`, where nothing
   is sent, promote in ~8 s because failover has to wait out the ack and
   witness timeouts. This is the single largest real-vs-loopback
   difference in the failover path.
2. **Timeouts tuned on loopback held.** No false failovers: netem up to
   20 ms never tripped `failover_ms` 1000, and all three hosts being in one
   datacentre (RTT ~0.34 ms) left the connect/retry/witness timeouts with
   wide margin. The one place a timeout bit was a loopback-only *test*
   deadline (`test_runtime`, fixed). Left untested: cross-datacentre RTT,
   where these margins would shrink.
3. **Sync replication got cheaper, not dearer.** 0.98-1.05x of non-sync
   durable, vs 0.54-0.60x on loopback: the standby runs on its own host so
   the shared-core contention is gone, and the replication RTT overlaps the
   fsync. The ratio moved the opposite way from a naive "fsync + RTT" guess.
4. **Whole-slot copies in one admin line do not scale — confirmed.** The
   100K-object / chunk-8 move did not complete (control plane lost after
   ~197 s, slot left sealed). chunk 256 still completes 100K in 11.3 s. The
   chunked stream is needed before slots this size migrate in production.
5. **Resume on reconnect:** the P4 10 s cut's ~26K-entry backlog drained in
   8.1 s after heal with 100K objects; a 1M-object reconnect on the 5-host
   layout is still untested (3 hosts cannot cut one node link without also
   failing over both groups).
6/7. Leases and NUMA: not stressed here (sub-ms drift, single-socket 2-vCPU
   hosts); left for a multi-socket / cross-site run.

**Open — intermittent balance-check failure under `partition`.** About 1
run in ~24 (seen once in ~24 `partition` runs; `kill`/`stop`/`oneway`/soak
never showed it), the check fails: balances net to 0 (money conserved) but
~32 accounts do not equal their acknowledged transfers, and the client
recorded fewer in-doubt transfers (14) than needed to explain them. It
appears only under `partition`, where both groups fail over at once (both
span a and b) during the long ~8 s dual-promote window. The witness Paxos
was seen to behave correctly in every inspected run (it refused the
cut-off old primary's degrade — "NO term 0 was replaced by 1" — so that
primary blocked its pending writes and withdrew, no split brain). Net-0
narrows it to either (a) a lost or double-applied *acknowledged* transfer
— a real but rare safety bug; a candidate is that a request's target
account and its client-session object can live on different shards whose
per-shard replication streams are cut at different points, so a replayed
request re-applies — or (b) the client (`snc_load`) counting an in-doubt
transfer as acknowledged. `check()` now preserves `.delta`/`.unknown` on
failure so the next occurrence can be classified. **Do not treat failover
under partition as proven until this is pinned down.**

**Cause found (2026-11-04, after the run): a resend answered
`E_OVERLOADED`.** `Shard::execute` refused new work when the shard's
follow-up backlog was over its limit *before* checking the client's
session. A promoted primary starts with a large backlog (resume re-sends
every unsettled transfer) at exactly the moment clients resend their
unanswered requests. A resend of a transfer the old primary had already
replicated got `E_OVERLOADED` instead of its recorded answer; the client
treats `E_OVERLOADED` as "never ran" and sends it again as a new request, so
it ran twice. That matches every symptom: net 0 (each double is a whole
transfer), more off accounts than in-doubt transfers, and only under
`partition`, whose ~8 s dual failover builds the largest backlog. Fixed: a
sequenced mutation checks its session before the overload refusal (the
same ordering rule `E_MOVED` already follows). Regression test:
`test_transfer` `test_resend_overloaded` (fails without the fix).
Re-run on fresh hosts: 24 of 24 `p3 --fault partition` rounds passed, with
the limits given in "Partition re-run with the fix" above.
