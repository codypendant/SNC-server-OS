#!/bin/bash
# Read-only root capture for the R640 storage/session-state investigation
# (docs/handoff/PR112_CP1A_Duration_Final_Review.md). Run as: sudo bash capture-root.sh
#
# Every command only reads: SCSI LOG SENSE / MODE SENSE / INQUIRY (smartctl -x,
# sg_logs -a, sdparm, sg_vpd), the ext4 superblock (tune2fs -l), and the kernel
# ring buffer (dmesg). No counter is reset (no sg_logs -R, no smartctl -t/-X/-s),
# nothing is written to the device, no trim, discard, format or mode select.
set -u
OUT="$(cd "$(dirname "$0")" && pwd)/capture"
mkdir -p "$OUT"
run() {
  local f=$1; shift
  { echo "\$ $*"; echo "# captured $(date -u +%FT%TZ) as $(id -un)"; "$@" 2>&1; echo "# rc=$?"; } > "$OUT/$f.txt"
  echo "wrote $f.txt"
}
run smartctl-x-sdb      smartctl -x /dev/sdb
run sg-logs-all-sdb     sg_logs -a /dev/sdb
run sg-logs-list-sdb    sg_logs /dev/sdb
run sg-vpd-sdb          sg_vpd -a /dev/sdb
run sdparm-all-sdb      sdparm --all /dev/sdb
run sdparm-cache-sdb    sdparm --page=ca /dev/sdb
run tune2fs-sdb1        tune2fs -l /dev/sdb1
run dmesg               dmesg -T
run smartctl-x-sda      smartctl -x /dev/sda
chown -R "${SUDO_USER:-solo}:${SUDO_USER:-solo}" "$OUT"
echo done
