# Campaign preparation: manifests and the first-wave gap inventory

**Status (2026-11-06):** work order `handoff/PR53_Review_Handoff.txt`.

- **CORRECTNESS BASELINE: READY FOR CAMPAIGN PREPARATION.**
- **FULL EXPERIMENTAL BASELINE: NOT YET FROZEN.**

This document covers four things:

1. what the main R640's manifest records, and how to capture it;
2. the first-wave gap inventory;
3. a minimal plan for the gaps the pilot needs;
4. the order of work to the freeze.

It authorizes nothing by itself, and it contains no comparative
results. Every gap below is read from the repository (INFERRED, EL0).

## 1. Main R640 manifest

**Not captured yet.** The R640 is cody's physical host, and this
project's cloud sessions cannot reach it. The manifest is measured on
the host itself, by `bench/manifest.sh`; nothing here is inferred from
memory. cody runs the script below and returns its output file.

**What `bench/manifest.sh` records** (extended in this slice to cover
the work order's list):

| Work order item | Recorded by |
| --- | --- |
| CPU model, topology; sockets, cores, threads | `lscpu`, `lscpu -e` |
| NUMA | `numactl --hardware`; per-node memory and huge pages (`/sys/devices/system/node`) |
| Memory | `free -h`, `/proc/meminfo`; BIOS and DIMMs (size, speed, rank) through `dmidecode` when run as root |
| OS, kernel | `uname -a`, `/etc/os-release`, `/proc/cmdline`, `numa_balancing`, `swappiness`, dirty ratios, isolated and `nohz_full` CPUs |
| Storage, filesystem, mounts | `lsblk`, `nvme list`, the data directory's `df`/`findmnt` (mount options), and per disk: scheduler, write cache, FUA, queue depth |
| NIC, link | `ethtool` (speed, driver, firmware), `ip -br link`, MTU, the NIC's NUMA node |
| IRQ, RSS | `ethtool -l` (channels), `-x` (RSS indirection), `-k` (offloads), `-c` (coalescing), `-g` (rings); each NIC IRQ's CPU affinity; whether irqbalance is running |
| THP, huge pages | THP `enabled` and `defrag`; `HugePages_*` |
| Governor, frequency | the governor per CPU; the frequency driver, min/max, `intel_pstate` status and `no_turbo`, boost; idle states and their latencies |
| Compiler, build environment | `c++`, `cmake`, `ninja`, `python3`, glibc versions |
| Experiment network topology | a section the operator fills in by hand: which hosts share which switch or link, link speeds, and each host's role |

**To run on the R640** (as root, from a checkout of `main`; NAME, DATA_DIR
and the NIC names are the host's own):

```sh
cd SNC-server-OS && git pull     # your checkout of the repository
sudo apt-get install -y numactl ethtool nvme-cli dmidecode pciutils util-linux   # Debian/Ubuntu; dnf on RHEL-likes
ip -br link                      # pick the test NIC(s)
findmnt /                        # pick the filesystem the SNC data dirs will use
sudo sh bench/manifest.sh r640-main /path/to/snc-data eno1 \
  > bench/manifests/r640-main-$(date -u +%Y%m%d).txt
```

Then fill in the topology section by hand. Remove any public address of
a host that is not disposable. Commit the file, or send it back for
review.

**Remote R640:** the same procedure, plus RTT, jitter and loss to the
main host (`bench/lab/lab.py inventory p0`). It is due before any WAN or
remote-site run, not before a local pilot that does not use it.

## 2. First-wave gap inventory

**What the first wave needs.** The pilot (EXPERIMENTAL_BASELINE 10.1)
runs workload A at one fixed setting, 5 times per system, on the host
it will run on, and records the CV of throughput and of p99. The primary
metric (10.2) is throughput per core at a p99 objective. The secondary
metrics are memory per object, WAL and network bytes per operation,
failover and recovery interruption, the E_MIGRATING window, and the
complexity count.

**Proposed first-wave scope:** workload A only.

- **Systems:** SNC (Profiles A and B), PostgreSQL and Redis, whose
  harnesses exist. SpacetimeDB is decision D1 below.
- **Second wave:** workloads B and C (X2), and Orleans, Restate and DBOS
  (X3). None of them is needed for the pilot or for setting N, M_B, the
  Path B thresholds or the p99 objective.

| # | Gap | Exists today | Needed for | Class |
| --- | --- | --- | --- | --- |
| G1 | **Pilot runner:** one fixed workload A setting, repeated 5 times per system, with CV of throughput and of p99, raw output kept | Single runs: `net_bench`, `bench/conventional/run.py`, `run_replication.py` | Pilot | **REQUIRED** |
| G2 | **Competitor tail latency.** `run.py` reads only pgbench's *average* latency, and Redis's p50. The primary metric needs p99 (and p99.9) from every system | SNC: `net_bench` prints p50 to p99.9 | Pilot, primary metric | **REQUIRED** |
| G3 | **Per-run resource metrics, the same way for every system:** CPU per op (process CPU time over the run), RSS per object, storage bytes written per op, network bytes per op | Pieces: SNC `RESOURCES` (`rss_kb`), `bench/lab/cpu_sample.py` | Secondary metrics; Path B | **REQUIRED** (harness only: `/proc/<pid>/stat`, `/proc/<pid>/io`, interface counters; no server change) |
| G4 | **X4 run tagging** for Profile B: every interval tagged healthy, degraded/lost standby, or failover/recovery from sampled STATUS, and never pooled | `lab.py` polls STATUS in the background; no tagging rule | Every Profile B run (run validity rule 2) | **REQUIRED** before any Profile B result |
| G5 | **Index-query mix in the SNC driver.** X1 put `INDEX` on the wire; no driver sends it | `net_bench` mixes reads, deposits and transfers | Workload A as specified ("secondary-index queries") | **REQUIRED** for workload A; competitor side: a PostgreSQL index on email; Redis has no secondary index, so it is a semantics-table mismatch or a client-maintained index, stated as such |
| G6 | **Local/remote ownership knob** | `net_bench --route owner/any`, `--misroute PCT` (non-owner shard); in-process sweeps (CROSSCORE.md) | Workload A locality axis | **EXISTS** for the pilot's fixed setting; cross-machine classes come with the lab |
| G7 | **Allocation rule:** the same CPU list, memory limit, device and network path for each system | CPU pinning: `net_bench --cpus`, `snc_server --cpus`, `run.py --server-cpus/--client-cpus`; no memory limit | Every comparison | **PARTIAL:** add a memory limit (cgroup v2 `systemd-run -p MemoryMax=`) to the runner |
| G8 | **Semantics and mismatch table** per system | CONVENTIONAL.md, REPLICATION.md prose | CRITERIA.md, run validity rule 4 | **REQUIRED** (a document, in CRITERIA.md) |
| G9 | **Complexity inventory** (services, configuration surface, deployment steps, operator actions) | None | Secondary metric, Path B | **REQUIRED** before CRITERIA.md (a document) |
| G10 | **SpacetimeDB harness** (X3) | None | First-wave comparison if D1 says so | **DECISION D1** |
| G11 | **Workload B and C drivers** (X2) | Primitives only: `create_near`, follow-ups, slot moves, `resume` | Second wave | DEFERRED |
| G12 | **Orleans, Restate and DBOS harnesses** (X3) | None | Second wave (B, C) | DEFERRED |
| G13 | **Disk-full/EIO injection; randomized long fault runs** | Lab P3 to P6, soak | Failure program | DEFERRED (not needed for the pilot) |

## 3. Minimal plan (for review; nothing here is built yet)

**CP-1: pilot and measurement harness.** One slice: harness and driver
code only, with no change to `snc_server`, the witness or the wire.

- G1: `bench/pilot.py`. It runs one system at one fixed setting N times
  under the allocation rule (G7, with a cgroup memory limit). It keeps
  every run's raw output and writes `pilot.json`: per-run throughput,
  p50 to p99.9, CV of throughput and of p99, and the G3 metrics.
- G2: PostgreSQL p99/p99.9 from pgbench's per-transaction log (`-l`),
  and Redis's from its percentile output, replacing the averages.
- G3: process CPU time, RSS (or SNC's `rss_kb`), bytes written and
  network bytes, sampled before and after each run, divided by
  operations.
- G4: X4 tagging in `lab.py`. Classes come from the STATUS samples it
  already takes. The summary reports each class separately, plus the
  share of time in each.
- G5: `net_bench --index PCT`, which sends `INDEX` lookups on the email
  index for that share of operations, and the PostgreSQL equivalent.
- **Tests:** unit tests for the CV, tagging and per-op arithmetic, and a
  loopback smoke run per system. All results stay on the dev VM and
  count as regression checks only.

**D1 (decision for cody and Codex):** is SpacetimeDB in the first wave?

- **Yes:** a separate X3a slice, a SpacetimeDB module for the accounts
  workload with its semantics stated, before the pilot runs it.
- **No:** the first wave is SNC against PostgreSQL and Redis, and
  SpacetimeDB moves to the second wave with B.

**Recommendation:** no. The first wave is then SNC against PostgreSQL
and Redis, which already have harnesses, so the pilot can set N and the
p99 objective sooner. The risk: workload A's named comparison stays
incomplete until SpacetimeDB's harness exists.

**Documents before CRITERIA.md:** the semantics and mismatch table (G8)
and the complexity inventory (G9).

## 4. Order to the freeze

1. cody captures the main R640's manifest (section 1); it is committed.
2. Review this inventory: decide D1, authorize CP-1.
3. CP-1, with tests, reviewed.
4. The within-system pilot on the R640. Raw output is committed; it
   sets N, M_B's floor, the Path B thresholds' inputs and the p99
   objective. No decision rule moves because of it.
5. `bench/results/<campaign>/CRITERIA.md` is committed, with the frozen
   SNC SHA, profiles, competitor versions and configurations, semantics
   and mismatch tables, metrics, N, M_B, each Path B threshold, the p99
   objective and the GO/REFINE/NARROW/STOP rules.
6. Freeze the SHA. No feature work lands on it during the campaign; a
   campaign-forced correctness fix is an explicit REFINE round.
7. Only then, the first comparative run.

Still frozen: Documents, generic Events/Timers, WaitSets, Activation,
the Agent Runtime, snapshot/PITR/branch implementation, security and
capabilities, multi-tenancy, a Rust rewrite, DPDK/VPP, S2/S3, the
recovering acceptor, unrelated optimization.
