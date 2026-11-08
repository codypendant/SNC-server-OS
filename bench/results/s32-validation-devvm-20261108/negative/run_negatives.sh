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
# 3. The same with the structural check also removed (on_shard and
#    submit_), and the permit replaced by an uncounted park: stop() no
#    longer waits for the slot_report inside, and returns while it is
#    parked; released, it would submit to a stopped worker and wait on a
#    latch nobody counts down (the abandoned waiter). L2's "stop waits"
#    probe catches it before the hang.
mutate slot-report-ungated-unchecked L2 src/core/engine.cpp release \
  "SlotReport Engine::slot_report(u16 slot) {
  Op op(*this);
  if (!op || !running_) return SlotReport{};" \
  "SlotReport Engine::slot_report(u16 slot) {
  if (!running_) return SlotReport{};
  g_debug_ops_parked.fetch_add(1);
  while (g_debug_park_ops.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  g_debug_ops_parked.fetch_sub(1);" \
  "  using R = decltype(fn());
  require_op();" \
  "  using R = decltype(fn());" \
  "void Engine::submit_(u16 shard, std::function<void()> fn) const {
  require_op();" \
  "void Engine::submit_(u16 shard, std::function<void()> fn) const {"
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
# 6. A failed start lands in RUNNING or CONSTRUCTED again (the pre-review
#    code): a half-started engine admits operations and a second start.
mutate failed-start-runs L9 src/core/engine.cpp release \
  "    life_ = Lifecycle::FAILED;" \
  "    life_ = running_ ? Lifecycle::RUNNING : Lifecycle::CONSTRUCTED;"
# 7. The nested mark is single-slot (only the innermost engine): A -> B -> A
#    takes a fresh, counted A permit, refused while A is STOPPING.
mutate nesting-single-slot L10 src/core/engine.cpp release \
  "    for (int i = 0; i < n; ++i)
      if (e[i] == x) return true;
    return false;" \
  "    return n > 0 && e[n - 1] == x;"
# 8. stop() from a shard thread is not detected: it waits for the
#    operation that waits on that shard (the child's alarm ends it).
mutate stop-on-shard-unchecked L11 src/core/engine.cpp release \
  "  if (current_shard() != nullptr) {
    std::fprintf(stderr, \"snc: Engine::stop() called from a shard thread (would wait for itself)\\n\");
    std::abort();
  }
" \
  ""
echo done >> "$OUT/summary.txt"
