#!/bin/bash
# WAL capacity fix: each mutation must make the named test fail.
# Usage: run_negatives.sh REPO WORK OUT_DIR   (builds patched copies in WORK)
set -u
REPO=$1; WORK=$2; OUT=$(realpath -m "$3")
mkdir -p "$OUT"
echo "base $(git -C "$REPO" rev-parse HEAD)" > "$OUT/summary.txt"
mutate() {  # name test old new
  local name=$1 test=$2
  rm -rf "$WORK/src-$name" "$WORK/build-$name"
  mkdir -p "$WORK/src-$name" && git -C "$REPO" archive HEAD | tar -x -C "$WORK/src-$name"
  python3 - "$WORK/src-$name/src/core/storage.cpp" "$3" "$4" <<'PY' || { echo "$name NOT APPLIED" >> "$OUT/summary.txt"; return; }
import sys
p, old, new = sys.argv[1:4]
s = open(p).read()
assert s.count(old) == 1, "pattern not unique"
open(p, 'w').write(s.replace(old, new))
PY
  cmake -S "$WORK/src-$name" -B "$WORK/build-$name" -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
  cmake --build "$WORK/build-$name" --target test_wal_capacity -j4 > "$OUT/$name-build.log" 2>&1 || { echo "$name BUILD FAILED" >> "$OUT/summary.txt"; return; }
  (cd "$WORK/build-$name" && timeout 300 ./test_wal_capacity "$test" > "$OUT/$name-$test.log" 2>&1)
  local rc=$?
  echo "$name $test rc=$rc (expected non-zero)" >> "$OUT/summary.txt"
  rm -rf "$WORK/src-$name" "$WORK/build-$name"
}
# M1. No maximum: growth never refuses (the defect's shape, minus the wrap).
mutate no-max C3 '  if (cap64 == 0) {' '  if (false) {'
# M2. append() sizes the record in u32 again (wraps for a ~4 GiB payload).
mutate u32-size C3 '  ensure_capacity(size64);  // fail-stop if it cannot fit: size64 <= max < 2^32 below' '  ensure_capacity(record_size(payload_len));'
# M3. append_raw() skips the capacity check.
mutate raw-unchecked C4 '  ensure_capacity(h.total_len);  // fail-stop if it cannot fit' '  if (u64{active_len_} + h.total_len + kBlock > active_cap_) ensure_capacity(0);'
# M4. Growth forms the sum before checking (wraps near 2^64).
mutate sum-first C1 '  if (len > max || extra > max - len || kBlock > max - len - extra) return 0;' '  if (len + extra + kBlock > max) return 0;'
# M5. The open check on max_active_bytes is removed.
mutate no-open-check C5 '  if (cfg_.max_active_bytes < kWalActiveInitial || cfg_.max_active_bytes > kWalActiveHardMax) {' '  if (false) {'
echo done >> "$OUT/summary.txt"
