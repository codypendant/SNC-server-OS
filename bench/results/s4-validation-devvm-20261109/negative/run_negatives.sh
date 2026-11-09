#!/bin/bash
# S4 negative mutations (docs/S4_SYNC_TOKENS.md section 7, S4-T10): each mutation
# of the token code must make the named test fail. Builds a patched
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
  [ $? -eq 0 ] || { echo "$name MUTATION NOT APPLIED (pattern did not match)" >> "$OUT/summary.txt"; rm -rf "$WORK/src-$name"; return; }
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
TARGET=test_s4_sync_tokens
# N1. Bypass: a ready part syncs without waiting for its token (the token
#     is still counted, so only the independent in-flight count sees it).
mutate token-bypass T1 src/core/shard.cpp release \
  "    if (!sync_tokens_->try_acquire(c.ticket)) return;" \
  "    sync_tokens_->try_acquire(c.ticket);"
# N2. K+1: the allocator grants one ticket past the window.
mutate k-plus-one T1 src/core/sync_tokens.h release \
  "if (limit != 0 && ticket >= releases.load(std::memory_order_acquire) + limit) return false;" \
  "if (limit != 0 && ticket > releases.load(std::memory_order_acquire) + limit) return false;"
# N3. Leak: a completed sync never returns its token.
mutate token-leak T2 src/core/shard.cpp release \
  "    c.token_held = false;
    sync_tokens_->release();
  }" \
  "    c.token_held = false;
  }"
# N4. Leak on the fatal path: the release moves after the failure check,
#     so a failed sync aborts still holding its token.
mutate fatal-path-leak T6 src/core/shard.cpp release \
  "  if (c.token_held) {
    c.token_held = false;
    sync_tokens_->release();
  }
  if (result < 0) {
    errno = -result;
    storage_fatal(\"checkpoint fdatasync failed: \" + c.path);
  }" \
  "  if (result < 0) {
    errno = -result;
    storage_fatal(\"checkpoint fdatasync failed: \" + c.path);
  }
  if (c.token_held) {
    c.token_held = false;
    sync_tokens_->release();
  }"
# N5. Unfair: grants by free capacity, not ticket order (still at most K
#     as long as no two shards race the same free slot).
mutate unordered-grant T3 src/core/sync_tokens.h release \
  "if (limit != 0 && ticket >= releases.load(std::memory_order_acquire) + limit) return false;" \
  "(void)ticket; if (limit != 0 && grants.load() - releases.load(std::memory_order_acquire) >= limit) return false;"
# N6. CC-3 dropped: the ticket (and the sync) no longer wait for the
#     part's WAL to be durable here.
mutate cc3-durable-check-dropped T3 src/core/shard.cpp release \
  "  if (c.writes_inflight == 0 && lsn_local_durable(c.max_lsn)) {
    const u64 now = sri_now_ns();" \
  "  if (c.writes_inflight == 0) {
    const u64 now = sri_now_ns();"
echo done >> "$OUT/summary.txt"
