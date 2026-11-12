# A2-a preregistration: persistent device identity correction (2026-11-12 morning)

Filed verbatim from cody's upload in the project thread, 2026-11-12 15:44 UTC.
The morning preflight for the PR #114 A2-a run refused after a reboot moved the data SSD from sdb to sda.

---

The morning preflight did the correct thing and stopped.

Do not reboot again merely to obtain "/dev/sdb".

Linux "/dev/sdX" enumeration is not a stable hardware identity, and the reboot has exposed a defect in the A2-a preregistration:

- approved data SSD: Samsung "ARFX3840S5xnFTRI"
- serial: "S48LNA0M700325"
- filesystem UUID: "3bf857c8-0cb4-4c57-b9a5-14328e70b061"
- current boot enumeration: SSD = "/dev/sda", Seagate HDD = "/dev/sdb"

The existing driver/sudoers/guard hardcode "sdb" and "/dev/sdb1", so they must not run on this boot.

Required correction

Patch the A2-a preregistration to use persistent device identity rather than "/dev/sdX".

Partition identity

Resolve the data filesystem from:

"/srv/snc-data"

and require its filesystem UUID to be exactly:

"3bf857c8-0cb4-4c57-b9a5-14328e70b061"

Do not require the partition to be named "sdb1".

Physical SSD identity

Resolve the mounted partition to its parent physical disk dynamically.

Require the resolved disk to match the approved Samsung SSD by persistent hardware identity:

- model "ARFX3840S5xnFTRI"
- serial "S48LNA0M700325"

Also capture and pin the WWN / "/dev/disk/by-id" identity if exposed on this host.

The runtime "/dev/sdX" name is evidence only and may differ across boots.

Root commands

Update the read-only SMART / SCSI commands and sudoers rule to use a verified persistent "/dev/disk/by-id/..." device path rather than "/dev/sdb".

Update "tune2fs" to use the stable filesystem/device identity rather than "/dev/sdb1".

"fstrim -v /srv/snc-data" is already mount-based and should remain so.

Do not guess the "/dev/disk/by-id" name. Capture the actual symlinks on this boot first and use the verified one.

Diskstats

Diskstats still uses kernel names, so dynamically resolve the approved physical SSD's current kernel block name at startup/session guard time.

Example concept:

persistent SSD identity → current kernel name ("sda" today) → "/proc/diskstats"

The current kernel name must never itself be the identity check.

Guard

Every session must verify that:

1. "/srv/snc-data" has the approved filesystem UUID.
2. Its parent physical disk has the approved Samsung model/serial/WWN.
3. The dynamically resolved kernel device used for diskstats is that same physical disk.
4. None of those mappings changed during the five-session invocation.

If they do:

INCOMPLETE — STOP.

Tests

Add deterministic tests/mutations for:

- approved SSD presented as "sda";
- approved SSD presented as "sdb";
- HDD occupying "sdb";
- wrong filesystem UUID;
- correct mount but wrong physical serial;
- correct serial but wrong mounted filesystem;
- device-name change during a session;
- persistent by-id path resolving to the wrong disk;
- diskstats reading the dynamically resolved approved disk.

The suite must prove there is no remaining semantic dependency on the literal strings "/dev/sdb", "/dev/sdb1", or kernel name "sdb" for device identity.

Status

No trim occurred this morning.

No benchmark occurred.

A0 remains intact:

- "fstrim.timer" disabled/inactive;
- "fstrim.service" inactive;
- no trim since the prior recorded one.

Therefore no experimental data was contaminated.

This is a preregistration/harness correction discovered by a fail-closed morning preflight.

Return the updated PR for independent review before installing sudoers or running A2-a.