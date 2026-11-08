#!/bin/bash
# S3.3 negative mutations (docs/ENGINE_LIFECYCLE.md sections 9-10): each mutation
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
# R4. The data-plane lease counts nothing: stop no longer waits for it.
mutate lease-uncounted R src/core/engine.cpp release \
  "  if (life_ != Lifecycle::RUNNING) return DataPlane(nullptr);
  ++active_ops_;" \
  "  if (life_ != Lifecycle::RUNNING) return DataPlane(nullptr);" \
  "  if (--e->active_ops_ == 0) e->life_cv_.notify_all();
}

Engine::DataPlane& Engine::DataPlane::operator=" \
  "}

Engine::DataPlane& Engine::DataPlane::operator="
# R4b. A lease is granted in STOPPING too (the gate checks the runtime flag).
mutate lease-in-stopping R src/core/engine.cpp release \
  "  if (life_ != Lifecycle::RUNNING) return DataPlane(nullptr);" \
  "  if (!running_) return DataPlane(nullptr);"
# C5. promote() without the durable-control lock: it runs inside a held
#    checkpoint (order A).
mutate promote-unserialized C1 src/core/engine.cpp release \
  "  std::lock_guard<std::mutex> control(control_mu_);
  if (!running_ || !standby_() || cfg_.data_dir.empty()) {" \
  "  if (!running_ || !standby_() || cfg_.data_dir.empty()) {"
# C5b. checkpoint() without it: it commits inside a parked promotion
#    (order B).
mutate checkpoint-unserialized C1 src/core/engine.cpp release \
  "  std::lock_guard<std::mutex> control(control_mu_);
  if (cfg_.data_dir.empty() || !running_) return rep;" \
  "  if (cfg_.data_dir.empty() || !running_) return rep;"
echo done >> "$OUT/summary.txt"
