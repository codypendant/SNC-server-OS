#!/bin/bash
# S3.1 negative mutations (docs/S3_WRITEBACK.md section 10): each mutation
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
  cmake --build "$WORK/build-$name" --target test_s3_writeback -j4 > "$OUT/$name-build.log" 2>&1 || { echo "$name BUILD FAILED" >> "$OUT/summary.txt"; return; }
  (cd "$WORK/build-$name" && ulimit -f 2097152 && SNC_SANITIZED=1 ASAN_OPTIONS=symbolize=0 timeout 600 ./test_s3_writeback "$test" > "$OUT/$name-$test.log" 2>&1)
  local rc=$?
  echo "$name $test ($flavour) rc=$rc (expected non-zero)" >> "$OUT/summary.txt"
  rm -rf "$WORK/src-$name" "$WORK/build-$name"
}
# 1. stop() does not wait for a checkpoint in progress.
mutate stop-no-wait T14 src/core/engine.cpp release \
  "  std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
  if (!running_) return;" \
  "  if (!running_) return;"
# 2. The same through the destructor.
mutate destroy-no-wait T16 src/core/engine.cpp release \
  "  std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
  if (!running_) return;" \
  "  if (!running_) return;"
# 3. checkpoint() does not hold the lifecycle lock (stop runs under it).
mutate checkpoint-unlocked T15 src/core/engine.cpp release \
  "  std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
  if (cfg_.data_dir.empty() || !running_) return rep;" \
  "  if (cfg_.data_dir.empty() || !running_) return rep;"
# 4. The engine state goes back to a static owner, freed at process exit
#    under the running shard threads (the PR #77 use-after-free).
mutate static-state T16b src/core/engine.cpp asan \
  "  state_ = std::make_unique<EngineState>();" \
  "  state_ = std::make_unique<EngineState>(); static std::unique_ptr<EngineState> s_owner; s_owner.reset(state_.get());"
# 5. stop() does not wait AND the shard quiesce ignores an active
#    checkpoint: the runtime drain is then the only thing keeping the
#    shard alive, and it no longer covers the checkpoint, so stop returns
#    with the checkpoint unfinished (T14's "stop has not returned").
mutate no-wait-no-drain T14 src/core/engine.cpp release \
  "  std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
  if (!running_) return;" \
  "  if (!running_) return;" \
  @src/core/shard.cpp \
  "waiters_.empty() && !ckpt_.active &&" \
  "waiters_.empty() &&"
echo done >> "$OUT/summary.txt"
