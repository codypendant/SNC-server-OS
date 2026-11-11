# Main R640: topology review and proposed pilot allocation

*Status: OBSERVED facts from two manifests, both captured by Claude Code
running on the host itself:*
- *`bench/manifests/r640-main-20261106.txt`, untouched, 05:17 UTC at 58c638f;*
- *`bench/manifests/r640-main-20261106b.txt`, 06:00 UTC at c1a2383, after
  cody added a SAS SSD as the data disk and moved eno1 to a 10 GbE switch.*

*Nothing was tuned between the two manifests. **Manifest b describes the
host the campaign uses.*** INFERRED and
PROPOSED items are marked. **Nothing here is approved.** The allocation
(section 4) and the decisions (section 5) wait for cody/Codex review.
CP-1A is not authorized.*

Remembered specifications are not evidence. The manifest shows **320 GB
of RAM, not the ~384 GB remembered**, and **no NVMe**.

> **Manifest e supersedes manifest d for all future evidence (2026-11-11,
> section 2d; capture APPROVED in review).** cody added DIMMs: 12 × 32 GB
> DDR4-2400 2-rank, 384 GB nominal, 376 GiB usable. A1-A6 and B1-B6 are
> populated, six of six channels per socket. The 5-of-6-channel limitation
> below applies only to manifest d's configuration. Manifest d and the
> CP-1A and diagnosis results stay as historical evidence for that old
> configuration; they are not reinterpreted under manifest e.

*Update after manifest b, 2026-11-06:* the data dir is now on a dedicated
3.84 TB 12 Gb/s SAS SSD, and eno1 links at 10 Gb/s. Section 5's storage
decision is settled, and gap R-G4 is closed.

## 1. What the host is (OBSERVED)

| Area | Observed |
|---|---|
| Platform | Dell PowerEdge R640, BIOS 2.21.2 (2024-02-19); bare metal (`systemd-detect-virt`: none); hostname solo4 |
| CPU | 2 × Intel Xeon Gold 6150 (Skylake-SP, 18 cores, 2.7 GHz base); SMT on; 72 logical CPUs |
| NUMA | 2 nodes, distance 10/21. **Node 0 = even CPUs, node 1 = odd CPUs.** SMT siblings are CPU *c* and *c*+36 (same core and cache ids in `lscpu -e`) |
| Caches | L2 1 MiB per core; L3 24.75 MiB per socket |
| Memory | 314 GiB usable: 10 × 32 GB DDR4-2400, 2-rank, configured at 2400 MT/s; 24 slots, 14 empty; about 161 GB per node |
| Huge pages | none reserved; THP `madvise` (defrag `madvise`) |
| Data disk (manifest b) | `sdc` = **Samsung ARFX3840S5xnFTRI, 3.84 TB SAS SSD**, firmware 3P00, SAS link negotiated 12.0 Gbit, 512 B logical / 4096 B physical sectors. It sits on the LSI SAS3008 HBA (18:00.0, root port 17:00.0, NUMA node 0). One GPT partition, ext4 with default options (`rw,relatime,stripe=2`; stripe is ext4's own default from the device geometry), mounted at `/srv/snc-data`. Volatile write cache on (`WCE 1`, the device default), `fua=1`, mq-deadline, `rotational=0` |
| Storage (manifest a) | **No NVMe.** `sdb` = DELLBOSS VD 447 GB (the BOSS M.2 card, Marvell 88SE9230 at 5e:00.0), holding `/boot/efi`, `/` (ext4, `rw,relatime`) **and the data dir `/srv/snc-data`**; `write_cache=write back`, `fua=0`, mq-deadline, reports `rotational=1`. `sda` = Seagate ST3000DM008 3 TB 7200 rpm desktop HDD with an old `VMFS_volume_member` partition. An LSI SAS3008 HBA (18:00.0) is present |
| NICs | 4 × Intel X550 10GBASE-T ports (two dual-port controllers, 1a:00 and 1c:00), ixgbe driver. `eno1` linked at 1000 Mb/s in manifest a and **at 10000 Mb/s in manifest b** (now on a 10 GbE switch), MTU 1500, NUMA node 0, 63 combined queues. It carries the default route and management traffic on a shared LAN. `eno2`-`eno4` have no link |
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
- **INFERRED: memory bandwidth is below the platform's maximum.**
  *(Manifest d's configuration only. Superseded by manifest e, section
  2d: six of six channels.)* 5 DIMMs
  per socket on a 6-channel controller leaves at least one channel
  empty per socket, and the DIMMs run at 2400, not the 6150's 2666. This
  is the same for every system, but it caps memory-bound results.
  **Gap R-G2:** the manifest does not say which slots are populated; the
  iDRAC hardware inventory does.
- **Durable profiles fsync to the SAS SSD** (manifest b), which is
  dedicated to data and not shared with the OS. Durable claims say
  "12 Gb/s SAS SSD", not NVMe-class.
  - INFERRED: an enterprise SAS SSD of this class usually has
    power-loss protection, which would make its write cache safe for
    fsync. That is not verified, so the device's flush latency is
    measured, never assumed.
  - It is the same device for every system in a comparison. Changing it
    later invalidates the durable pilot.
  - Gap R-G3 (the BOSS RAID level) only matters for the OS disk now.
- **Still no physical-network evidence.** eno1 now links at 10 Gb/s
  (manifest b), but there is still one experiment host. The link is
  also shared with the default route and management traffic.
  Pilot client traffic is loopback and is labelled loopback (CAMPAIGN_PREP
  section 4). Profile B (sync HA) and any physical-network number need a
  second host and a dedicated link: a direct cable between an X550 port
  (eno2-eno4, 10GBASE-T) and the other host is the simplest.
- **Mitigations are expensive on Skylake** (PTI, IBRS). Syscall-heavy
  systems pay more. They are recorded and left as they are; changing
  them would be a named configuration, not a silent default.
- **SMT siblings share a core.** Pinning two busy threads to *c* and
  *c*+36 halves each one's core. SMT use must be explicit in every run.

## 2a. BIOS System Profile as found (OBSERVED, 2026-11-06, gap R-G1 closed)

This was read in iDRAC (Configuration → BIOS Settings → System Profile
Settings) by cody and photographed:
`bench/manifests/r640-main-20261106-bios-system-profile.jpg`. Nothing
was changed.

| Setting | Value |
|---|---|
| System Profile | **Performance Per Watt (DAPC)** |
| CPU Power Management | System DBPM (DAPC) |
| Memory Frequency | Maximum Performance |
| Turbo Boost | Enabled |
| C1E | Enabled |
| C States | Enabled |
| Write Data CRC | Disabled |
| Memory Patrol Scrub | Standard |
| Memory Refresh Rate | 1x |
| Uncore Frequency | Dynamic |
| Energy Efficient Policy | Balanced Performance |
| Turbo Boost enabled cores, processor 1 / 2 | All / All |
| Monitor/Mwait | Enabled |
| Workload Profile | Not Available |
| CPU Interconnect Bus Link Power Management | Enabled |
| PCI ASPM L1 Link Power Management | Enabled |

DAPC (BIOS-controlled P-states) explains why the OS exposes no cpufreq
driver. **PROPOSED:** use Dell's "Performance" preset for the campaign.
It keeps turbo on and turns off C-states and C1E, sets uncore to
maximum, and disables link power management. Changing it needs a reboot
and a third manifest, and waits for cody's decision.

## 2b. Manifest c: after the BIOS switch to Performance (OBSERVED)

`bench/manifests/r640-main-20261106c.txt` was captured at 06:37 UTC, at
2e232bf. Before the capture, the R640 session had set
`kernel.numa_balancing=0`.

- **Frequency is now under OS control.** `intel_pstate` runs in passive
  mode with the `intel_cpufreq` driver. The governor is `schedutil` on
  all 72 CPUs, the range is 1.2-3.7 GHz, and turbo is on
  (`no_turbo=0`). Pinning the frequency policy for the campaign is open
  (CP1A_DRAFT_REVIEW F2).
- **Idle states.** C6 is now disabled. POLL, C1 and C1E are still
  offered to the OS.
- **Disk names moved at reboot.** The data SSD is now `sda`, the old
  HDD is `sdb`, and the BOSS is `sdc`. `/srv/snc-data` mounts by UUID,
  so nothing broke. The `/dev/sdc` in the copied "extra" section refers
  to the names in manifest b. Name devices by model or UUID, never by
  letter.
- Everything else matches manifest b.

## 2c. Manifest d: governor `performance` (OBSERVED)

`bench/manifests/r640-main-20261106d.txt` was captured at 12:59 UTC at
5bcaca2 by the R640 session. It ran only the governor change and the
capture. SHA-256:
`2784c2e4bf8b0f52b2584a0359509cbe629c65c6ed36c793b49c80a151d8b81b`.

- **Governor:** `performance` on all 72 CPUs, set at run time with
  sysfs. It does not survive a reboot. Every CPU read 3.4-3.7 GHz at the
  capture (turbo on).
- **`kernel.numa_balancing` is 1 again.** It was 0 in manifest c. That
  setting is also run-time only, so most likely the host rebooted between
  the captures (inferred: the manifest records no uptime). It must be 0
  during runs (section 4). `bench/pilot.py` refuses to start otherwise.
  Like the governor, it is set before each campaign session and checked
  by the runner; neither is made persistent.
- **DIMM slots (gap R-G2 closed):** A1-A5 and B1-B5 each hold one 32 GB
  2-rank DDR4 at 2400 MT/s. A6-A12 and B6-B12 are empty.
  - INFERRED from Dell's R640 slot layout (A1-A6 and B1-B6 are the first
    slot of the six channels per socket): five of six memory channels
    per socket are populated, one DIMM per channel, and one channel per
    socket is empty.
  - Memory bandwidth is therefore about 5/6 of a fully populated
    socket at 2400 MT/s. That is the same for every system compared on
    this host. cody keeps this configuration fixed for the campaign.
- Idle states are as in manifest c (C6 disabled; POLL, C1, C1E offered).
- The disk names are as in manifest c (the SSD is `sda`). Name devices
  by model or UUID.
- The hand-filled network section is empty in d. Section 2 and manifest
  c's notes still apply: eno1 is at 10 Gb/s on a shared LAN, and there
  is one host.

**ACCEPTED** (docs/handoff/PR61_Addendum_ManifestD.txt): manifest d is
the campaign manifest, and its SHA-256 is `approved_manifest_sha256` in
`bench/pilot_plan.json`.

**Before every evidence session**, set and verify the governor and
`numa_balancing`. Neither survives a reboot:

```
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor > /dev/null
sudo sysctl -w kernel.numa_balancing=0
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | sort | uniq -c   # 72 performance
cat /proc/sys/kernel/numa_balancing                                           # 0
```

`bench/pilot.py` refuses to start if either is wrong.

## 2d. Manifest e: all six memory channels populated (OBSERVED; capture APPROVED)

`bench/manifests/r640-main-20261111.txt` was captured at 13:55 UTC
on 2026-11-11 by the R640 session, at d0d3bb7 (merged main, PR #95). It
ran only the update to main, the governor and `numa_balancing`
settings, and the capture. SHA-256:
`768056bbee1d7f178ed0be1c195b821f0638a1de28a4aca1ea0afdf5444c27b6`.

| Area | Manifest d | Manifest e |
|---|---|---|
| DIMMs | 10 × 32 GB DDR4, 2-rank, 2400 MT/s: A1-A5, B1-B5 (320 GB nominal) | **12 × 32 GB** DDR4, 2-rank, 2400 MT/s: **A1-A6, B1-B6** (384 GB nominal); A7-A12 and B7-B12 empty |
| Channels per socket (INFERRED from Dell's R640 slot layout: A1-A6 and B1-B6 are the first slot of each of the six channels) | 5 of 6, one DIMM per channel | **6 of 6**, one DIMM per channel |
| MemTotal | 329,771,792 kB (314 GiB) | 394,799,836 kB (376 GiB) |
| NUMA node 0 / node 1 MemTotal | 164,684,076 / 165,087,716 kB | 196,637,644 / 198,162,192 kB |
| Governor | `performance` × 72 | `performance` × 72 |
| `kernel.numa_balancing` at capture | 1 | 0 |

Unchanged from manifest d:

- Platform and BIOS (Dell R640, BIOS 2.21.2).
- Kernel (Ubuntu 6.8.0-149-generic).
- CPUs: 2 × Gold 6150, 18 cores, SMT, 72 CPUs. Node 0 is the even CPUs,
  node 1 the odd.
- Scaling driver `intel_cpufreq`. HugePages 0.
- eno1: ixgbe, 10000 Mb/s full duplex, 63 combined channels.
- The data SSD: model ARFX3840S5xnFTRI, 3.5 T, ext4,
  `rw,relatime,stripe=2`, at `/srv/snc-data`.
- The list of manifest sections is identical (the section headers diff
  is empty).

Observed differences that are not hardware:

- **Disk names moved again at the reboot.** The BOSS is now `sda`, the
  data SSD `sdb` and the old VMFS HDD `sdc`. Devices are named by model
  or UUID, never by letter.
- **`/srv/snc-data` shows 44 KB used**; manifest d showed 2.2 GB. Old
  data directories were removed, or the filesystem was recreated. The
  manifest cannot tell which, and cody should confirm. Either way it is
  the same disk model, mount and options.
- Per-CPU MHz readings differ (turbo on, 3.4-3.7 GHz); that is expected.

What it means:

- **Memory bandwidth (INFERRED).** With six channels the theoretical
  peak per socket is 6/5 of manifest d's. The DIMMs still run at
  2400 MT/s, not the 6150's 2666, so this is not the platform maximum.
  Results from manifest d and manifest e are different machine
  configurations: no comparison across them without saying so.
- **Allocations are unchanged** (PILOT-L and PILOT-X,
  `bench/pilot_plan.json`), and so is the data on the SAS SSD. Each
  node now has about 192 GB.
- **Manifest e is the campaign manifest.** `bench/pilot_plan.json`
  pins its SHA-256 (the manifest-baseline closure, docs/PILOT_PLAN.md
  section 2). Manifest d is retired for future evidence.
  - `bench/pilot.py` compares only the file passed with `--manifest`,
    not the live hardware. The manifest must therefore be recaptured
    and reviewed after any hardware change, as this one was.
- CP-1A and S6 evidence still need their own explicit authorization.

## 3. Locality (OBSERVED, with one inference)

- **OBSERVED (manifest b, sysfs `numa_node`):** the SAS3008 HBA
  (18:00.0) that carries the data SSD, the BOSS controller (5e:00.0) and
  both X550 controllers (1a:00.0, 1c:00.0) are all on **node 0**. Gap
  R-G4 is closed.
- So both I/O paths that matter (the data disk and the NIC) are local to
  **node 0**.

## 4. PROPOSED pilot allocation (for review)

*Superseded by the PR #61 review.* The table below put the client on
node 1. That is now the named cross-socket allocation **PILOT-X**. The
baseline is **PILOT-L**: server and client on disjoint node 0 cores. Both
are defined in `bench/pilot_plan.json` and `docs/PILOT_PLAN.md` section 2.

For the SNC-only pilot, Profile A (LOCAL_DURABLE) and Profile C (memory),
on this host only:

| Role | CPUs | Memory | Notes |
|---|---|---|---|
| Housekeeping, OS, IRQs | 0 and its sibling 36 | - | nothing under test runs here |
| SNC server shards | node 0 physical cores, one thread each: 2, 4, …, 34 (up to 17 shards) | `--membind=0` | local to the data SSD's HBA and to eno1. The shard-count sweep uses 1, 2, 4, 8, 16 |
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

1. **Storage for durable profiles: SETTLED** (cody, 2026-11-06). cody
   added a dedicated 3.84 TB 12 Gb/s SAS SSD ("the fastest you're gonna
   get"; no NVMe is available). Durable claims are labelled "SAS SSD".
2. **The BIOS System Profile: SETTLED.** It was recorded as DAPC
   (section 2a). cody switched it to "Performance" (manifest c, section
   2b). The PR #61 review sets the governor to `performance` for the
   campaign; that change is recorded in manifest d.
3. **Network:** eno1 is now on a 10 GbE switch. The pilot is still
   loopback-only, because there is one experiment host. Cross-host work
   waits for a second host on that switch, and ideally a dedicated port
   (eno2-eno4).
4. **The old HDD** (ST3000DM008; `sda` in manifest d, `sdc` in
   manifest e) holds an old VMFS volume. It stays untouched and is never
   used by the campaign unless cody says otherwise.
5. **The deterministic knee algorithm** goes into the CP-1A plan after
   this review (EXPERIMENTAL_BASELINE 10.2).
