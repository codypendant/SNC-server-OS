# TB-0 result: R640 data SSD (card `docs/r640/TB0_DEVICE_FACTS.md`)

The raw output is on branch `r640-dc1-tb0`, commit 9d39ecc, in `bench/results/dc1-trackb/tb0-r640-device/`. It was taken on 2026-11-22 at 21:23 UTC on kernel 6.8.0-149-generic. Labels follow `docs/ENGINEERING_PRINCIPLES.md`.

## Device
- FACT (sg_inq, smartctl): SAMSUNG ARFX3840S5xnFTRI, firmware 3P00, 3.84 TB SAS-3 at 12 Gbps.
  - The model matches the card's expected model.
  - SMART health is OK, with 0% endurance used, 0 grown defects and 0 uncorrected errors.
- FACT (sysfs): write_cache is "write back", fua is 1, rotational is 0 and the scheduler is mq-deadline.
- FACT (sdparm, Caching mode page): WCE=1, NV_DIS=0, DPOFUA=1.
- FACT (sg_vpd, Extended INQUIRY): NV_SUP=1 and V_SUP=1. The device reports both a non-volatile and a volatile cache, so NV_SUP alone does not prove the write cache is protected.
- Gap: the card's output does not record the findmnt/lsblk line that tied /srv/snc-data to this device. The fio files are on /srv/snc-data/dc1-tb0, and the only device matching the expected model is this one. Next cards record that line.

## Flush cost (fio, 4 KiB random writes, O_DIRECT, QD1, 30 s each)

| job | IOPS | write p50 / p99 / p99.9 µs | sync p50 / p99 / p99.9 µs |
|---|---|---|---|
| none | 28,117 | 34 / 51 / 64 | n/a |
| sync=dsync (FUA writes) | 20,613 | 33 / 173 / 228 | n/a |
| fdatasync each write | 6,289 | 42 / 79 / 95 | 103 / 166 / 288 |

FACT: a FUA write costs the same as a plain write at the median, and the flush costs about 0.1 ms.

## Power-loss protection
- EXT, Samsung PM1643 brochure: "Upon detection of a failure, the SSD immediately uses the stored energy from tantalum capacitors to provide enough time to transfer the cached data in DRAM to the flash memory, ensuring no loss of data." (https://download.semiconductor.samsung.com/resources/brochure/PM1643%2012G%20SAS%20SSD.pdf)
- EXT, PM1643 datasheet rev 1.0: "PM1643 supports Power Loss Protection which can guarantee that data issued by the host system are written to the storage media without any loss." (https://files.hddguru.com/download/Datasheets/Samsung/pm1643_datasheet.pdf)
- EXT, a reseller listing only: Dell part MZILT3T8HBLSAD3 is called "Samsung PM1643a". No Samsung or Dell document found here maps ARFX3840S5xnFTRI to the PM1643a. (https://serverpartdeals.com/products/dell-samsung-pm1643a-mzilt3t8hblsad3-0crnph-3-84tb-1-dwpd-sas-12gb-s-512e-2-5-solid-state-drive)
- INF: this drive has capacitor-backed power-loss protection. Three things support it:
  - the vendor family documents;
  - the device reports NV_SUP=1;
  - the flush timings above. A 0.1 ms flush and a free FUA are what a protected cache looks like, while an unprotected NAND flush takes milliseconds.
- **Status: UNVERIFIED.** The above is inference only. Only a power cut during acknowledged writes would prove it, and that test is not planned, because it risks the lab's only data disk. The plan's UNRESOLVED item stays UNRESOLVED.

## What it means for Track B (`docs/DIRECTION_COMPARISON_PLAN.md` 4.1)
- The durability test does not depend on this result. dm-log-writes models losing every volatile cache at each mark, whatever the drive does. So the VM `fua=0` path and the R640 `fua=1` path are judged by the same rule.
- These are device costs for one fio pattern (4 KiB, QD1), not a system's cost. Each system issues its own mix and size of writes, flushes and FUA writes, and batches them in its own way: the same disk does not give the same overhead. Each system's flush and FUA behaviour is measured on the R640 for its own configuration (4.1: a `blktrace` sample per configuration), and none is assumed from this table.
- The 4.1 UNRESOLVED items are unchanged: whether the SSD has power-loss protection, and the `fua=1` versus `fua=0` path difference.
- Correction, 2026-11-22 (Codex review, `docs/handoff/PR185_191_Codex_Review.md`): the first version said the flush cost "is the same for every system, so it biases no comparison", and marked power-loss protection INF. Both are withdrawn. The measurements are unchanged.
