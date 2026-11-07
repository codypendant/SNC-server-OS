#!/bin/bash
S=/tmp/claude-0/-home-claude-snc-server-os/507fa1ab-0d45-51b5-8435-5e5cb510f86d/scratchpad
cd /home/claude/snc-server-os; O=$S/hm2; mkdir -p $O; SUM=$O/summary.txt; : > $SUM
# Negative paths
for m in absent stale; do python3 $S/c2_negative.py build $m > $O/neg-rel-$m.log 2>&1; echo "neg-rel-$m $(tail -1 $O/neg-rel-$m.log)" | tee -a $SUM; done
SNC_SANITIZED=1 python3 $S/c2_negative.py build-asan absent > $O/neg-asan-absent.log 2>&1; echo "neg-asan-absent $(tail -1 $O/neg-asan-absent.log)" | tee -a $SUM
# Stressed parallel rounds: lab guard (Release) + c2 (ASan) + test_sync_readiness (Release), two CPU burners
for r in 1 2 3; do
  python3 -c "while True: pass" & B1=$!; python3 -c "while True: pass" & B2=$!
  python3 tests/test_lab_witness_guard.py build > $O/stress$r-lab.log 2>&1 & P1=$!
  SNC_SANITIZED=1 python3 tests/test_ready_order.py build-asan c2_restart_held > $O/stress$r-c2.log 2>&1 & P2=$!
  python3 tests/test_sync_readiness.py build > $O/stress$r-sync.log 2>&1 & P3=$!
  wait $P1; a=$?; wait $P2; b=$?; wait $P3; c=$?
  kill $B1 $B2; wait $B1 $B2 2>/dev/null
  echo "stress$r lab=$a c2=$b sync_readiness=$c" | tee -a $SUM
done
ps -eo comm | grep -cE "snc_|net_bench" | sed 's/^/stray_after_stress=/' | tee -a $SUM
(time ctest --test-dir build -j1 --output-on-failure) > $O/full-release-j1.log 2>&1
grep -E "tests passed" $O/full-release-j1.log | tee -a $SUM
echo PHASE2_DONE | tee -a $SUM
