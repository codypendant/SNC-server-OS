#!/bin/bash
# Per-step timing of the unchanged walk (bpftrace uprobes, step_probe.bt). Evidence only.
set -u
E=bench/results/r640-s2-pacing-20261111; OUT=$E/probe; BIN=$(realpath build-s6/ckpt_walk_cost)
D=/srv/snc-data/walkcost-20261111/n625000; BT=$OUT/probe.resolved.bt
mkdir -p $OUT; sed "s#BIN#$BIN#g" $E/step_probe.bt > $BT
S() { sudo "$@"; }  # needs root for bpftrace (as run: sudo password supplied on stdin; not recorded)
echo "start $(date -u +%FT%TZ) head=$(git rev-parse HEAD) bin_sha256=$(sha256sum $BIN | cut -d' ' -f1)" > $OUT/runs.txt
for r in 1 2 3; do for P in L2:2,4 L0:2; do for m in off 1 5 10 20 25 50 100 250; do
  pl=${P%%:*}; cpus=${P#*:}; f=$OUT/$pl-$m-p$r
  S bpftrace -o $f.events -c "numactl --physcpubind=$cpus --membind=0 $BIN $D 0 $m" $BT > $f.stdout 2>&1
  gzip -f $f.events; echo "$(date -u +%T) $pl $m p$r rc=$?" >> $OUT/runs.txt
done; done; done
echo "end $(date -u +%FT%TZ)" >> $OUT/runs.txt
