# Main R640: topology review and proposed pilot allocation

*Status: OBSERVED facts from the untouched manifest
(`bench/manifests/r640-main-20261106.txt`, captured 2026-11-06 05:17 UTC
at 58c638f by Claude Code running on the host itself). INFERRED and
PROPOSED items are marked. **Nothing here is approved.** The allocation
(section 4) and the decisions (section 5) wait for cody/Codex review.
CP-1A is not authorized.*

Remembered specifications are not evidence. The manifest shows **320 GB
of RAM, not the ~384 GB remembered**, and **no NVMe**.

## 1. What the host is (OBSERVED)

| Area | Observed |
|---|---|
| Platform | Dell PowerEdge R640, BIOS 2.21.2 (2024-02-19); bare metal (`systemd-detect-virt`: none); hostname solo4 |
| CPU | 2 × Intel Xeon Gold 6150 (Skylake-SP, 18 cores, 2.7 GHz base); SMT on; 72 logical CPUs |
| NUMA | 2 nodes, distance 10/21. **Node 0 = even CPUs, node 1 = odd CPUs.** SMT siblings are CPU *c* and *c*+36 (same core and cache ids in `lscpu -e`) |
| Caches | L2 1 MiB per core; L3 24.75 MiB per socket |
| Memory | 314 GiB usable: 10 × 32 GB DDR4-2400, 2-rank, configured at 2400 MT/s; 24 slots, 14 empty; about 161 GB per node |
| Huge pages | none reserved; THP `madvise` (defrag `madvise`) |
| Storage | **No NVMe.** `sdb` = DELLBOSS VD 447 GB (the BOSS M.2 card, Marvell 88SE9230 at 5e:00.0), holding `/boot/efi`, `/` (ext4, `rw,relatime`) **and the data dir `/srv/snc-data`**; `write_cache=write back`, `fua=0`, mq-deadline, reports `rotational=1`. `sda` = Seagate ST3000DM008 3 TB 7200 rpm desktop HDD with an old `VMFS_volume_member` partition. An LSI SAS3008 HBA (18:00.0) is present |
| NICs | 4 × Intel X550 10GBASE-T ports (two dual-port controllers, 1a:00 and 1c:00), ixgbe driver. **`eno1` is linked at 1000 Mb/s** (advertises 10G), MTU 1500, NUMA node 0, 63 combined queues. It carries the default route and management traffic on a shared LAN. `eno2`-`eno4` have no link |
| IRQs | 64 eno1 IRQ lines, each affinitized to all node 0 CPUs; irqbalance inactive |
| Frequency | **No cpufreq driver is exposed** (`scaling_driver`, limits, turbo: n/a). cpuidle `intel_idle` with POLL/C1/C1E/C6 (133 µs) all enabled |
| Kernel | Ubuntu 24.04.5, 6.8.0-149-generic, no extra cmdline; `numa_balancing=1`, `swappiness=60`, `dirty_ratio=20`, `sched_autogroup=1`; no isolcpus or nohz_full |
| Mitigations | PTI (Meltdown), IBRS (Retbleed and Spectre v2), MDS/TAA buffer clears, "SMT vulnerable" on several |
| Build env | gcc 13.3.0, cmake 3.28.3, ninja 1.11.1, Python 3.12.3, glibc 2.39 |

## 2. What it means for the campaign

- **INFERRED: frequency is under BIOS control.** No OS scaling driver
  usually means the Dell System Profile does not hand P-states to the OS
  (for example "Performance" or a custom profile). Turbo, C-state and
  uncore behaviour are therefore set in the BIOS and are not visible in
  the manifest. **Gap R-G1:** record the BIOS System Profile, turbo,
  C-states, C1E, uncore frequency, SNC (sub-NUMA clustering) and memory
  mode from iDRAC (System BIOS → System Profile Settings / Memory
  Settings, read only) before the pilot.
- **INFERRED: memory bandwidth is below the platform's maximum.** 5 DIMMs
  per socket on a 6-channel controller leaves at least one channel
  empty per socket, and the DIMMs run at 2400, not the 6150's 2666. This
  is the same for every system, but it caps memory-bound results.
  **Gap R-G2:** the manifest does not say which slots are populated; the
  iDRAC hardware inventory does.
- **Durable profiles would fsync to the BOSS card.** That is a SATA M.2
  device, probably two SSDs in RAID-1 (the BOSS default; INFERRED,
  **gap R-G3**), shared with the OS. Durable latency and throughput will
  be bounded by it, not by an NVMe-class device. It is the same device
  for every system, so the comparison stays fair, but the claim must say
  "SATA/BOSS durable", and a later change of device invalidates the
  durable pilot.
- **No network evidence is possible on this host as wired.** There is
  one host, and its only live link is a shared 1 GbE management LAN.
  Pilot client traffic is loopback and is labelled loopback (CAMPAIGN_PREP
  section 4). Profile B (sync HA) and any physical-network number need a
  second host and a dedicated link: a direct cable between an X550 port
  (eno2-eno4, 10GBASE-T) and the other host is the simplest.
- **Mitigations are expensive on Skylake** (PTI, IBRS). Syscall-heavy
  systems pay more. They are recorded and left as they are; changing
  them would be a named configuration, not a silent default.
- **SMT siblings share a core.** Pinning two busy threads to *c* and
  *c*+36 halves each one's core. SMT use must be explicit in every run.

## 3. Locality (OBSERVED, with one inference)

- `eno1` reports NUMA node 0.
- **INFERRED from PCI bus numbers:** the X550s (1a, 1c, under root ports
  on bus 17), the SAS3008 (18) and the BOSS controller (5e, under bus 5d)
  all hang off socket 0. Buses 80 and above belong to socket 1. **Gap
  R-G4:** confirm with `cat /sys/bus/pci/devices/0000:5e:00.0/numa_node`
  (and 18:00.0, 1c:00.0) on the host.
- So both I/O paths that matter (storage and the NIC) are local to
  **node 0**.

## 4. PROPOSED pilot allocation (for review)

For the SNC-only pilot, Profile A (LOCAL_DURABLE) and Profile C (memory),
on this host only:

| Role | CPUs | Memory | Notes |
|---|---|---|---|
| Housekeeping, OS, IRQs | 0 and its sibling 36 | - | nothing under test runs here |
| SNC server shards | node 0 physical cores, one thread each: 2, 4, …, 34 (up to 17 shards) | `--membind=0` | local to the BOSS card and eno1. The shard-count sweep uses 1, 2, 4, 8, 16 |
| Client load generator (TCP loopback runs) | node 1 physical cores: 1, 3, …, 35 | `--membind=1` | never overlaps the server. Every request crosses the socket link, and the run says so |
| Unused | node 0 siblings 38-70; node 1 siblings 37-71 | - | SMT on/off is a named dimension, not default |

Run-time settings, each applied per run and recorded in the run
configuration (with a second manifest after the first tuned boot):

- `kernel.numa_balancing=0` during runs;
- THP stays `madvise` (SNC's `RuntimeConfig::pages` chooses per run);
- no isolcpus, nohz_full or IRQ re-pinning in the first pilot, so the
  pilot measures the stock host. Isolation is a later named dimension;
- the BIOS profile as recorded under R-G1, unchanged during the
  campaign.

Cross-NUMA (server memory on node 1) is a named configuration, run on
purpose and never left to the scheduler.

## 5. Decisions for cody (before CP-1A)

1. **Storage for durable profiles.** Choose one:
   - accept the BOSS card, and label every durable claim
     "SATA/BOSS, shared with the OS"; or
   - add an NVMe device (a U.2 drive if the backplane supports it, or a
     PCIe add-in card) **before** the pilot, so the pilot is not
     invalidated later.
   - *Claude recommends* deciding this before the pilot; NVMe if
     durable results should say anything about modern hardware.
2. **The BIOS System Profile:** record it (R-G1). If it changes for the
   campaign (for example to "Performance"), take a second manifest.
3. **Network:** the pilot is loopback-only. Cross-host work waits for a
   second host and a dedicated 10GbE link.
4. **The `sda` disk** holds an old VMFS volume. It stays untouched and
   is never used by the campaign unless cody says otherwise.
5. **The deterministic knee algorithm** goes into the CP-1A plan after
   this review (EXPERIMENTAL_BASELINE 10.2).
