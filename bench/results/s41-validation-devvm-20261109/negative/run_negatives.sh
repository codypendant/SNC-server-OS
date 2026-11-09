#!/bin/bash
# S4.1 negative mutations (docs/S4_SYNC_TOKENS.md section 9): each mutation
# of the admin reply path must make the named test fail. Builds a patched
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
TARGET=test_admin_loop
# N7. The old path: one non-blocking send, then close, whatever was sent.
mutate one-call-send A1 tools/admin_loop.h release \
  "      case Sent::BLOCKED: break;" \
  "      case Sent::BLOCKED: ::close(fd); return;"
# N8. A blocking write-all on the loop thread: a slow reader holds HEALTH.
mutate blocking-write A1 tools/admin_loop.h release \
  "      case Sent::BLOCKED: break;" \
  "      case Sent::BLOCKED: ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK); ha::write_all(fd, o.buf.substr(o.off)); ::close(fd); return;"
# N9. No write deadline: a stalled reader's reply waits forever.
mutate no-deadline A2 tools/admin_loop.h release \
  "          if (now >= o.deadline_ns) {" \
  "          if (false) {"
# N10. No bound on waiting replies.
mutate no-bound A3 tools/admin_loop.h release \
  "    if (out_.size() >= out_max_) {" \
  "    if (false) {"
echo done >> "$OUT/summary.txt"
