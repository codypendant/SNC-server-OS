#!/bin/bash
# Repeated runs of the two fixed tests. Usage: harness_matrix.sh OUTDIR
cd /home/claude/snc-server-os
O=$1; mkdir -p $O; SUM=$O/summary.txt; : > $SUM
run() { # label build env-sanitized cmd...
  local label=$1 b=$2 san=$3; shift 3
  local t0=$(date +%s)
  if [ "$san" = 1 ]; then SNC_SANITIZED=1 "$@" > $O/$label.log 2>&1; else "$@" > $O/$label.log 2>&1; fi
  local rc=$?
  local reps=$(grep -cE "WARNING: ThreadSanitizer|ERROR: AddressSanitizer|runtime error:" $O/$label.log)
  echo "$label rc=$rc sanitizer_reports=$reps secs=$(( $(date +%s)-t0 ))" | tee -a $SUM
}
for i in $(seq 1 10); do run rel-lab-$i build 0 python3 tests/test_lab_witness_guard.py build; done
for i in $(seq 1 10); do run rel-c2-$i build 0 python3 tests/test_ready_order.py build c2_restart_held; done
for i in $(seq 1 5); do run asan-lab-$i build-asan 1 python3 tests/test_lab_witness_guard.py build-asan; done
for i in $(seq 1 5); do run asan-c2-$i build-asan 1 python3 tests/test_ready_order.py build-asan c2_restart_held; done
for i in $(seq 1 5); do run tsan-lab-$i build-tsan 1 python3 tests/test_lab_witness_guard.py build-tsan; done
for i in $(seq 1 5); do run tsan-c2-$i build-tsan 1 python3 tests/test_ready_order.py build-tsan c2_restart_held; done
echo MATRIX_DONE | tee -a $SUM
