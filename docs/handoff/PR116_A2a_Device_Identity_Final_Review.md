# PR #116 A2-a revision 4 (persistent device identity): final review

Filed verbatim from cody's upload in the project thread, 2026-11-12 16:02 UTC
(relayed because the reviewer's GitHub connector is read-only).

---

PR #116 FINAL REVIEW

Repository:

"codypendant/SNC-server-OS"

PR:

"#116 — A2-a preregistration revision 4: persistent device identity"

Reviewed head:

"c9ae0b8697a60332fb8ae89bb003ff8e3dadd197"

Base:

"12be3716284cde38ae1f346dbff0379cd4c763ba"

Decision

PR #116: APPROVED.

MERGE AUTHORIZED.

PERSISTENT DEVICE-IDENTITY CORRECTION: ACCEPTED.

A2-a STORAGE-STATE REPRODUCIBILITY RUN: GO AFTER MERGE AND HOST PREFLIGHT.

---

Device identity

Approved data filesystem UUID:

"3bf857c8-0cb4-4c57-b9a5-14328e70b061"

Approved physical SSD:

Model:  ARFX3840S5xnFTRI
Serial: S48LNA0M700325
WWN:    0x5002538b097f68a0

Persistent whole-disk path:

"/dev/disk/by-id/wwn-0x5002538b097f68a0"

Persistent partition path:

"/dev/disk/by-id/wwn-0x5002538b097f68a0-part1"

Filesystem path:

"/dev/disk/by-uuid/3bf857c8-0cb4-4c57-b9a5-14328e70b061"

These identities are accepted.

---

Kernel enumeration

Current boot:

Samsung data SSD = sda
data filesystem  = sda1

Seagate HDD      = sdb
BOSS boot disk   = sdc

This is valid.

The experimental method must not require a particular "/dev/sdX" assignment.

No reboot is required or desired merely to change kernel enumeration.

---

Identity resolution accepted

The driver now:

1. resolves "/srv/snc-data";
2. verifies the approved filesystem UUID;
3. resolves the mounted partition;
4. resolves its physical parent disk;
5. verifies model;
6. verifies serial;
7. verifies WWN;
8. verifies the by-id whole-disk link;
9. verifies the by-id partition link;
10. verifies the by-UUID link;
11. verifies "pilot.disks_of()" identifies the same physical disk.

Only after all of those checks does it use the current kernel disk name for "/proc/diskstats".

The kernel name is evidence, not identity.

Accepted.

---

Mapping stability

Within one A2-a invocation, the resolved:

- source;
- partition kernel name;
- physical-disk kernel name;

must remain unchanged.

A mapping change during the five sessions causes:

INCOMPLETE — STOP.

The mapping may legitimately differ after a later reboot and future invocation provided the persistent identity still matches.

---

In-session rechecks

Accepted.

Identity is re-resolved and checked:

- after explicit trim;
- after the 30-minute settle;
- before the A1 end snapshot.

Therefore diskstats windows are only accepted if they correspond to the verified approved SSD.

---

Root command identity

Accepted root commands use persistent paths:

fstrim -v /srv/snc-data

smartctl -x /dev/disk/by-id/wwn-0x5002538b097f68a0

sg_logs -a /dev/disk/by-id/wwn-0x5002538b097f68a0

tune2fs -l /dev/disk/by-id/wwn-0x5002538b097f68a0-part1

The sudoers file contains no dependency on "sda", "sdb", "/dev/sda", or "/dev/sdb".

Accepted.

---

Test coverage

Revision-4 identity suite covers:

- approved SSD as "sda";
- approved SSD as "sdb";
- HDD occupying the other name;
- wrong filesystem UUID;
- wrong serial;
- wrong model;
- wrong WWN;
- correct serial with wrong mounted filesystem;
- by-id disk resolving incorrectly;
- by-id partition resolving incorrectly;
- by-UUID resolving incorrectly;
- runtime mapping change;
- unresolved mount;
- decoy diskstats entries;
- fixed-name diskstats implementations;
- missing identity rechecks;
- kernel-name use in root commands/sudoers.

Revision-4 mutation suite:

29 / 29 caught

The existing A1–A4 deterministic test logic remains in "test_a2a.py".

The older revision-3 mutation corpus was not rerun, but its underlying deterministic test coverage remains present. This is not a blocker.

---

Existing A2-a controls remain unchanged

Still required:

- K = 5 sessions;
- explicit proven fstrim each session;
- fail-closed pre-trim sync;
- fail-closed post-trim sync;
- 30-minute settle;
- zero data-SSD sectors written during settle;
- zero data-SSD sectors discarded during settle;
- A1 state captures;
- per-session environment guard;
- historical anchor = 654,569 ops/s;
- five fixed reps;
- five capacity reps;
- start/end sentinels;
- fixed 10% symmetric sentinel limit;
- within-session CV ≤10%;
- between-session CV ≤10%;
- supplemental multimodality rule;
- no replacement session;
- N_raw is output only.

No threshold changes.

---

Morning host state

The reboot changed two frozen host settings.

Before running A2-a restore:

kernel.numa_balancing = 0
all CPU governors     = performance

Use the same accepted R640 host-setup procedure used for CP-1A.

Do not change unrelated host settings.

The driver must independently verify both before execution.

---

A0

Continue to require:

fstrim.timer   = disabled / inactive
fstrim.service = inactive

Do not manually run fstrim before A2-a.

The experimental driver owns the five explicit trims.

---

Execution sequence after merge

1. Merge PR #116.
2. Pull the exact merge HEAD on the R640.
3. Make no further repository commit.
4. Restore:
   - CPU governors = "performance";
   - "kernel.numa_balancing = 0".
5. Confirm automatic fstrim remains disabled.
6. Install the narrow sudoers file:

sudo install -m 0440 bench/results/a2a-storage-repro-r640/sudoers-snc-a2a /etc/sudoers.d/snc-a2a
sudo visudo -c

7. Record the "visudo -c" result.
8. Run the committed pre-data test:

PYTHONDONTWRITEBYTECODE=1 python3 -B \
  bench/results/a2a-storage-repro-r640/test_a2a.py

9. If that passes, launch:

python3 -B bench/results/a2a-storage-repro-r640/a2a.py run \
  /home/solo/a2a-storage-repro-r640 \
  --scratch /dev/shm/snc-a2a \
  --manifest bench/manifests/r640-main-20261111.txt \
  --build build-s6

---

Failure discipline

If the driver reports:

INCOMPLETE

stop.

Do not fix the environment and resume at the next session.

Do not replace a session.

Do not bypass a guard.

If it reports:

STOP

preserve the complete result and return it for review.

If it reports:

STABLE

preserve the complete result and return it for review.

Do not automatically start the stable-peak bracket.

---

Still unauthorized

- stable-peak boundary validation;
- new Profile-A sweep;
- duration Latin square;
- CRITERIA freeze;
- competitor benchmarks;
- REFINE #2;
- performance tuning.

---

Final ruling:

PR #116 APPROVED.

MERGE IT.

DO NOT REBOOT TO CHANGE DISK NAMES.

RESTORE THE FROZEN HOST SETTINGS, THEN RUN THE FIVE-SESSION A2-a VALIDATION EXACTLY AS PREREGISTERED.