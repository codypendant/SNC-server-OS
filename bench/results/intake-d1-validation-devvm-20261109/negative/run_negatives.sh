#!/bin/bash
# D.1 negative mutations (docs/FOLLOWUP_INTAKE_DESIGN.md 3.9.5 and section 9):
# each mutation of the intake code must make the named test_intake case fail. Builds a patched
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
  echo "$name $test ($flavour) rc=$rc (expected non-zero): $(grep -m1 'CHECK failed' "$OUT/$name-$test.log")" >> "$OUT/summary.txt"
  rm -rf "$WORK/src-$name" "$WORK/build-$name"
}
TARGET=test_intake
F=src/core/shard.cpp
# N1. No WAL gate: follow-ups apply whatever the WAL holds.
mutate no-wal-gate F1 $F release \
  "  if (!intake_.wal_gate || !wal_) return true;" \
  "  if (true) return true;"
# N2. No window: a sender sends whatever the destination has staged.
mutate no-window F11 $F release \
  "  if (intake_.window == 0 || board_ == nullptr) return true;" \
  "  return true;"
# N4. The credit returned when an item arrives, not when it leaves staging.
mutate credit-at-drain F11 $F release \
  "  e.from = m.src_shard;  // owed a credit once it leaves staging (3.2)" \
  "  ++taken_from_[m.src_shard];
  e.from = kNoShard;"
# N6. No link pausing and no per-poll budget.
mutate no-link-pause F8 $F release \
  "  const u64 lmax = intake_.link_hold_max;" \
  "  const u64 lmax = 0;"
# N7. An arrival runs ahead of what is staged when the gate is open.
mutate no-staging-order F1 $F release \
  "  if (!emit_local_.empty() || gated) {" \
  "  if (gated) {"
# N8. No owner gate.
mutate no-owner-gate G1 $F release \
  "  if (!intake_.owner_gate || adm_.legacy || emits_in_flight() <= kEmitBacklog) return false;" \
  "  return false;"
# N9. No exchange budget X.
mutate no-exchange-budget F12 $F release \
  "  if (!intake_.exchange_gate) return false;" \
  "  return false;"
# N10. A follow-up dropped with no fail op never ends its exchange.
mutate drop-no-end F4 $F release \
  "  if (!emit_result(e, out.r.status) && !replied) end_exchange(e.origin);" \
  "  if (!emit_result(e, out.r.status) && !replied && out.r.status == Status::OK) end_exchange(e.origin);"
# N13. X checked on the static max_emits, not the invocation's reservation.
mutate static-reservation F12 $F release \
  "    if (client && c.emits_reserved_ != 0 && exchange_full()) {" \
  "    if (client && op->max_emits != 0 && exchange_full()) {"
# N14. No coalescing: every trigger restarts the scan.
mutate no-coalescing F12 $F release \
  "  if (scan_.active || scan_.owed_all || scan_.owed_any) ++stats_.resume_coalesced;" \
  "  if (scan_.active) {
    scan_.active = false;
    ++stats_.resume_restarts;
  }"
# N7b. Order only: an arrival skips what is staged whenever the gate is open
#      (the gate itself still holds), so per-source order breaks.
mutate staging-order-only F17 $F release \
  "  const bool gated = emit_local_.empty() && !followup_gate_open();
  if (!emit_local_.empty() || gated) {" \
  "  const bool gated = !followup_gate_open();
  if (gated) {"
# N15. The review's defect: a pass's re-emits wait only for the pass's base
#      position (0 after start), not for the WAL when the hook ran.
mutate resume-base-lsn F16 $F release \
  "    const u64 lsn = std::max(sc.base_lsn, wal_ ? wal_->next_lsn() - 1 : 0);" \
  "    const u64 lsn = sc.base_lsn;"
echo done >> "$OUT/summary.txt"
