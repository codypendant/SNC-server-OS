#!/bin/bash
# S3 negative mutations (docs/S3_WRITEBACK.md section 9): each mutation of
# the S3 code must make the named test fail. Builds a patched copy of the
# repository in WORK (outside the repo), Release, test_s3_writeback only.
# Usage: run_negatives.sh REPO WORK OUT_DIR
set -u
REPO=$1; WORK=$2; OUT=$(realpath -m "$3")
mkdir -p "$OUT"
echo "base $(git -C "$REPO" rev-parse HEAD)" > "$OUT/summary.txt"
mutate() {  # name test python-replacement-old python-replacement-new
  local name=$1 test=$2
  rm -rf "$WORK/src-$name"
  mkdir -p "$WORK/src-$name" && git -C "$REPO" archive HEAD | tar -x -C "$WORK/src-$name"
  python3 - "$WORK/src-$name/src/core/shard.cpp" "$3" "$4" <<'PY'
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
assert s.count(old) == 1, ("pattern not unique", old)
open(p, 'w').write(s.replace(old, new))
PY
  cmake -S "$WORK/src-$name" -B "$WORK/build-$name" -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
  cmake --build "$WORK/build-$name" --target test_s3_writeback -j4 > "$OUT/$name-build.log" 2>&1 || { echo "$name BUILD FAILED" >> "$OUT/summary.txt"; return; }
  (cd "$WORK/build-$name" && timeout 300 ./test_s3_writeback "$test" > "$OUT/$name-$test.log" 2>&1)
  local rc=$?
  echo "$name $test rc=$rc (expected non-zero)" >> "$OUT/summary.txt"
  rm -rf "$WORK/src-$name" "$WORK/build-$name"
}
# 1. The window never refuses.
mutate no-window T1 "  if (ckpt_io_.dsync && c.writes_inflight >= ckpt_io_.window) return false;" "  if (false) return false;"
# 2. Advisory window: a refused chunk does not stop the walk (keeps encoding).
mutate advisory-window T1 "          if (c.chunk.size() >= kCheckpointChunk && !checkpoint_submit_chunk()) c.blocked = true;" "          if (c.chunk.size() >= kCheckpointChunk) (void)checkpoint_submit_chunk();"
# 3. A completed chunk's buffer is kept until the checkpoint ends.
mutate no-release T2 "  std::vector<u8>().swap(w->buf);" ""
# 3b. FIFO free: a completion frees the oldest busy slot's buffer, not its own.
mutate fifo-free T4 "  std::vector<u8>().swap(w->buf);" "  { CkptWrite* v = w; for (auto& x : c.slots) if (x->busy && x->index < v->index) v = x.get(); std::vector<u8>().swap(v->buf); }"
# 4. The walk is done although the last chunk could not be submitted.
mutate final-early T5 "      if (!checkpoint_submit_chunk()) {
        c.blocked = true;" "      if (!checkpoint_submit_chunk() && false) {
        c.blocked = true;"
# 5. A positive short write is accepted.
mutate short-accepted T6 "  if (static_cast<u32>(result) != w->len) {" "  if (false) {"
# 6. The synchronous fallback is not counted.
mutate sync-uncounted T10 "    ++ckpt_obs_.sync_writes;
    ++ckpt_obs_.sync_writes_total;" ""
echo done >> "$OUT/summary.txt"
