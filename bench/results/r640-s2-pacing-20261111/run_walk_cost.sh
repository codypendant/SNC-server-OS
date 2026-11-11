#!/bin/bash
# R640 ckpt_walk_cost characterization (PR97 handoff, section C). Evidence only.
# Usage: run_walk_cost.sh OUTDIR   (run from the repo root; build-s6 at the frozen SHA)
set -u
OUT=$1; BIN=build-s6/ckpt_walk_cost; ROOT=/srv/snc-data/walkcost-20261111
MODES="off 10000000 1 2 5 10 20 25 50 100 250"
mkdir -p "$OUT" "$ROOT"
{ echo "date_utc: $(date -u +%FT%TZ)"; echo "git_head: $(git rev-parse HEAD)"; echo "git_dirty_files: $(git status --porcelain --untracked-files=no | wc -l)"
  echo "binary_sha256: $(sha256sum $BIN | cut -d' ' -f1)"; echo "build: $(grep CMAKE_BUILD_TYPE: build-s6/CMakeCache.txt)"
  echo "compiler: $(c++ --version | head -1)"; echo "data_root: $ROOT ($(findmnt -n -o SOURCE -T $ROOT))"; } > "$OUT/identity.txt"
run() { # placement label, numactl args, N, reps
  local P=$1 B=$2 N=$3 R=$4 D=$ROOT/n$3
  if [ ! -e "$D/.created" ]; then rm -rf "$D"; mkdir -p "$D"; echo "# create $N ($P)" >> "$OUT/create.log"
    numactl $B $BIN "$D" "$N" >> "$OUT/create.log" 2>&1 && touch "$D/.created"; fi
  for r in $(seq 1 $R); do for m in $MODES; do
    t0=$(date -u +%T); line=$(numactl $B $BIN "$D" 0 "$m" 2>&1)
    echo "$line" | sed "s/^/placement=$P N=$N proc=$r t=$t0 /" >> "$OUT/raw.txt"
  done; done
}
run L0 "--physcpubind=2 --membind=0" 625000 3      # primary: one server CPU of PILOT-L, node-0 memory
run L0 "--physcpubind=2 --membind=0" 1000000 3     # dev-VM comparison size
run X1 "--physcpubind=2 --membind=1" 625000 1      # sensitivity: remote memory
run L2 "--physcpubind=2,4 --membind=0" 625000 1    # sensitivity: worker not alone on its CPU set
echo "done $(date -u +%FT%TZ)" >> "$OUT/identity.txt"
