#!/bin/bash
# S3.2 negative mutations (docs/ENGINE_LIFECYCLE.md): each mutation
# of the lifecycle code must make the named test fail. Builds a patched
# copy of the repository in WORK (outside the repo): Release, or ASan+UBSan
# where the defect is a memory error only a sanitizer sees reliably.
# Usage: run_negatives.sh REPO WORK OUT_DIR
set -u
REPO=$1; WORK=$2; OUT=$(realpath -m "$3")
mkdir -p "$OUT"
echo "base $(git -C "$REPO" rev-parse HEAD)" > "$OUT/summary.txt"
mutate() {  # name test file flavour(release|asan) old new [old new]
  local name=$1 test=$2 file=$3 flavour=$4
  rm -rf "$WORK/src-$name" "$WORK/build-$name"
  mkdir -p "$WORK/src-$name" && git -C "$REPO" archive HEAD | tar -x -C "$WORK/src-$name"
  shift 4
  python3 - "$WORK/src-$name" "$file" "$@" <<'PY'
import sys
root, first, rest = sys.argv[1], sys.argv[2], sys.argv[3:]
# pairs: old new, optionally "@file" switching the file for later pairs
f = first
i = 0
while i < len(rest):
    if rest[i].startswith('@'):
        f = rest[i][1:]; i += 1; continue
    old, new = rest[i], rest[i + 1]; i += 2
    p = root + '/' + f
    s = open(p).read()
    assert s.count(old) == 1, ("pattern not unique", old)
    open(p, 'w').write(s.replace(old, new))
PY
  if [ "$flavour" = asan ]; then
    cmake -S "$WORK/src-$name" -B "$WORK/build-$name" -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS=-fsanitize=address,undefined > /dev/null 2>&1
  else
    cmake -S "$WORK/src-$name" -B "$WORK/build-$name" -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
  fi
  cmake --build "$WORK/build-$name" --target "$TARGET" -j4 > "$OUT/$name-build.log" 2>&1 || { echo "$name BUILD FAILED" >> "$OUT/summary.txt"; return; }
  (cd "$WORK/build-$name" && ulimit -f 2097152 && SNC_SANITIZED=1 ASAN_OPTIONS=symbolize=0 timeout 600 ./"$TARGET" "$test" > "$OUT/$name-$test.log" 2>&1)
  local rc=$?
  echo "$name $test ($flavour) rc=$rc (expected non-zero)" >> "$OUT/summary.txt"
  rm -rf "$WORK/src-$name" "$WORK/build-$name"
}
TARGET=test_lifecycle
# 1. The permit counts nothing: stop no longer waits for operations inside.
mutate permit-uncounted L1 src/core/engine.cpp release \
  "      if (e.life_ != Lifecycle::RUNNING) return;
      ++e.active_ops_;
    }
    ok_ = counted_ = true;" \
  "      if (e.life_ != Lifecycle::RUNNING) return;
    }
    ok_ = true;"
# 2. One representative on_shard path without its permit (slot_report):
#    the structural check catches it at once, with no stop needed.
mutate slot-report-ungated L2 src/core/engine.cpp release \
  "SlotReport Engine::slot_report(u16 slot) {
  Op op(*this);
  if (!op || !running_) return SlotReport{};" \
  "SlotReport Engine::slot_report(u16 slot) {
  if (!running_) return SlotReport{};"
# 3. The same with the structural check also removed: the abandoned
#    waiter the review names, under L8's concurrent stop (a slot_report
#    that saw the runtime running submits to a stopped worker).
mutate slot-report-ungated-unchecked L8 src/core/engine.cpp release \
  "SlotReport Engine::slot_report(u16 slot) {
  Op op(*this);
  if (!op || !running_) return SlotReport{};" \
  "SlotReport Engine::slot_report(u16 slot) {
  if (!running_) return SlotReport{};" \
  "  using R = decltype(fn());
  require_op();" \
  "  using R = decltype(fn());"
# 4. STOPPING still admits operations (the gate checks the runtime flag).
mutate stopping-admits L1 src/core/engine.cpp release \
  "      if (e.life_ != Lifecycle::RUNNING) return;
      ++e.active_ops_;" \
  "      if (!e.running_) return;
      ++e.active_ops_;"
# 5. stop() does not wait for a start in progress.
mutate stop-ignores-start L7 src/core/engine.cpp release \
  "    life_cv_.wait(lk, [&] { return life_ != Lifecycle::STARTING && life_ != Lifecycle::STOPPING; });" \
  "    life_cv_.wait(lk, [&] { return life_ != Lifecycle::STOPPING; });"
echo done >> "$OUT/summary.txt"
