#!/bin/bash
# S3-D micro-characterization matrix (docs/S3D_WRITEBACK.md). Dev VM only.
# Usage: run.sh BUILD_DIR DATA_DIR OUT_FILE. Each rep runs every cell once,
# in a rotated order, so drift is spread across cells.
set -u
B=$1; D=$2; OUT=$3
cells=(
  "none 0 512"
  "legacy 0 512" "c1 0 512" "c2 0 512" "c3 0 512"
  "legacy 100 512" "c1 100 512" "c2 100 512" "c3 100 512"
  "c3 100 512 4 256" "c1 100 512 1" "c2 100 512 1" "c3 100 512 1"
  "legacy 100 2048" "c1 100 2048" "c2 100 2048" "c3 100 2048"
)
n=${#cells[@]}
{
  echo "# sha $(git rev-parse HEAD) kernel $(uname -r) start $(date -u +%FT%TZ)"
  echo "# fs $(df -T "$D" | tail -1 | awk '{print $1, $2}') scheduler $(cat /sys/block/vda/queue/scheduler) write_cache $(cat /sys/block/vda/queue/write_cache)"
  echo "# dirty_ratio $(cat /proc/sys/vm/dirty_ratio) dirty_background_ratio $(cat /proc/sys/vm/dirty_background_ratio) mem_kib $(awk '/MemTotal/{print $2}' /proc/meminfo) nproc $(nproc)"
} > "$OUT"
for rep in 1 2 3; do
  for ((i = 0; i < n; i++)); do
    c=${cells[$(( (i + (rep - 1) * 5) % n ))]}
    line=$("$B/ckpt_writeback_char" run "$D" $c) || line="FAILED rc=$? cell=$c"
    echo "rep=$rep $line" >> "$OUT"
  done
done
echo "# end $(date -u +%FT%TZ)" >> "$OUT"
