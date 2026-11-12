# R640 storage/session-state investigation: evidence (2026-11-12), for review

Authorized by [docs/handoff/PR112_CP1A_Duration_Final_Review.md](../../../docs/handoff/PR112_CP1A_Duration_Final_Review.md)
("NEXT AUTHORIZED TASK"). **Read only.**
- No performance run.
- No trim, discard, format, reboot, preconditioning, mode select or counter reset.
- The device state is preserved.

The proposed method revision is in [METHOD_PROPOSAL.md](METHOD_PROPOSAL.md).

## Capture (`capture/`)

Captured 2026-11-12 03:20–03:33 UTC, with the host idle (load 0.00). The commands are listed at the top of each file.

| Area | Files |
|---|---|
| Device identity and state | `lsblk`, `lsscsi`, `sysfs-sdb` (queue, cache, provisioning, `stat`), `sg-vpd-sdb`, `sdparm-all-sdb`, `sdparm-cache-sdb` |
| SMART and logs (root, read only: LOG/MODE SENSE) | `smartctl-x-sdb`, `sg-logs-all-sdb`, `sg-logs-list-sdb`; boot disk `smartctl-x-sda` |
| Filesystem | `findmnt`, `df-h`, `df-i`, `tune2fs-sdb1` |
| Trim | `fstrim-timer`, `fstrim-service`, `fstrim-timers`, `journal-fstrim` |
| Host | `uptime`, `cpufreq`, `numa-balancing`, `meminfo`, `thermal`, `cmdline`, `uname`, `journal-boots` |
| Kernel events | `dmesg`, `journal-kernel-storage` |

The root part ran through `capture-root.sh`, which is kept here.

## Findings

### 1. The SSD was trimmed about 3 and 6 hours before the sessions, and not since

- `fstrim.service` ran 2026-11-11 15:08:23–15:09:17 UTC, the weekly `fstrim.timer` catching up 78 min after
  the 13:50 boot. It reported `/srv/snc-data: 3.4 TiB (3779298537472 bytes) trimmed on /dev/sdb1`.
- The device takes the trim. It reports UNMAP (`provisioning_mode: unmap`, `thin_provisioning: 1`,
  LBPRZ=1, discard granularity 8 KiB, max 4 GiB). The kernel's discard counter for `sdb` since boot is
  7,381,442,456 sectors (3.779 TB), which is that one trim.
- The filesystem is mounted `rw,relatime,stripe=2`, **without** `discard`. Freed blocks are therefore not
  trimmed continuously. Every run deletes its data dir, and those blocks stay mapped on the device until
  the next fstrim.
- **No trim between CP-1A and the duration validation.** The next timer trigger is
  **Mon 2026-11-16 00:56:44 UTC**. It would change the state this investigation is to preserve (see the
  proposal, A0).

Sessions on the data SSD, on one clock (UTC):

| Time | Event | Data-SSD host writes |
|---|---|---|
| 11-11 13:50 | boot | |
| 14:43–14:47 | S2 pacing re-derivation (before the trim) | |
| **15:09** | **fstrim: 3.78 TB unmapped** | |
| 18:25–19:51 | S6, write-window characterization, REFINE #1 | (not split here) |
| 21:18 | CP-1A starts (Profile A) | |
| 22:31–23:00 | **CP-1A A objective-rate reps (3 sweep + 5 fixed): p99 0.90–0.95 ms** | CP-1A cumulative 1.32–1.70 TB |
| 23:00–23:20 | rest of A (capacity, shard sweep) | +0.19 TB (A total 1.885 TB) |
| 23:21–00:28 | CP-1A Profile C | 0 (no SSD writes recorded) |
| 00:28–01:59 | idle, 91 min | |
| 01:59–02:23 | **duration validation: first 4 reps 1.05–1.18 ms, last 5 reps 1.47–1.54 ms** | 0.201 TB |
| 03:20 | this capture; no writes since | since boot: 7,473,308,656 sectors = **3.826 TB** |

- Host writes are the server's `/proc/<pid>/io` `write_bytes` (`write_bytes_per_op × served_all`) summed
  over runs, as in the PR #112 review: CP-1A A **1.885 TB**, duration validation **0.201 TB**.
- The device counter's 3.826 TB since boot also covers the S2 session before the trim and S6/W-char/REFINE #1
  after it. So **about 3.8 TB, roughly one drive capacity (3.84 TB), has been written since the trim, less
  S2's share.**

### 2. The p99 change does not follow cumulative writes alone

- CP-1A's 8 objective-rate reps were 0.90–0.95 ms at CP-1A cumulative writes of 1.32–1.70 TB, about
  1.5–1.9 TB after the trim.
- The validation's first rep came after only 0.19 TB more, plus 91 min idle and 67 min of
  Profile C. It was already 1.18 ms. Within the validation, the lowest rep (1.05 ms) came at 59 GB, and the
  step to 1.47 ms came between 70 GB and 107 GB.
- The step coincides with the first 240 s rep, at 02:07–02:12. The session is too short to separate
  duration, position and cumulative writes (PR #112 review, Finding 1).
- So the data fit a change of state somewhere between 23:00 and 02:07. Cumulative writes are one
  candidate, not a shown cause.

### 3. Device-reported state (SMART / SCSI log pages)

| Item | Value |
|---|---|
| Model / firmware | Samsung ARFX3840S5xnFTRI (SAS 12 Gb/s, 3.84 TB), rev 3P00, made 2019 week 30 |
| Health | `SMART Health Status: OK`; informational exception ASC/ASCQ 0/0 |
| Wear | percentage used endurance indicator **0%**; grown defects 0; pending defects 0 |
| Lifetime | power-on 8,611 h; host bytes written 17.106 TB, read 8.528 TB (ext4 lifetime writes since 11-06: 6,752 GB) |
| Errors | write / read / verify: 0 corrected, 0 uncorrected; non-medium error count 104 (lifetime, no timestamp); SAS phy: 0 invalid dword / disparity / sync loss / reset; 7 retry-class OPEN_REJECT |
| **Background medium scan** | **active, 79.61% progress**; 12 scans completed in 8,611 h; self-tests every ~700–750 h |
| Temperature | 36 °C now (reference 70 °C); since power-on min 29, **max 41 °C**; lifetime max 52 °C |
| Cache | write cache enabled (WCE=1, `write back`), read cache on, FUA 1; scheduler mq-deadline |
| Power states | 20 transitions to active, 0 to idle/standby: no power-state changes |

- **Background scan:** the device runs a background medium scan on its own schedule, and one is in progress
  now. There is no earlier snapshot of its progress, so **whether it ran during CP-1A, during the
  validation, or both is unknown**. It is a device-internal activity that competes with host I/O. It is a
  candidate mechanism, not a shown one.
- **Wear-levelling and GC:** no counter for internal garbage collection, free-block pool or write
  amplification is decoded on this drive. Vendor log pages 0x31 and 0x39 and vendor IE parameters are
  recorded as hex but not interpreted. **GC state is not observable here.**
- **Thermal:** the drive's maximum since power-on is 41 °C, well below its 70 °C reference. CPU package
  and PCH read 47–53 °C at idle (no lm-sensors). No thermal event was logged. Thermal throttling of the SSD
  is unlikely, but per-session temperatures were not recorded.

### 4. Host and kernel

- **No storage error since boot** (`dmesg`, kernel journal): no I/O error, reset, timeout or EXT4 error for
  `sdb` or its `mpt3sas` HBA (`0000:17/18:00.0`).
- **DMAR:** 30 logged and about 90,700 suppressed IOMMU DMA write faults at 15:09:11–15:09:16, all from
  `5e:00.0`. That device is the Marvell 88SE9230 behind the BOSS **boot** disk (`sda`), during the trim of
  `/` and `/boot/efi`. None came from the data SSD's path, and none after 15:09:16.
- Uptime 13 h 29 min (one boot covers every session); governors all `performance`; `numa_balancing` 0;
  390 GB available memory; swap unused; dirty 360 kB.

## What the evidence supports

- The two sessions ran in different device states. CP-1A ran 6–8 h after a full-device trim. The
  validation ran after about one more drive capacity of writes since that trim, mostly without continuous
  discard, and after an unmeasured background-scan state.
- The device exposes no error, wear or thermal change that would explain the shift.
- **Not shown:** that trim state, GC, the background scan or any other device-internal state causes the
  p99 change. This read-only snapshot, taken once after both sessions, cannot show it. Showing it needs
  controlled sessions, which are not authorized (see METHOD_PROPOSAL.md).
