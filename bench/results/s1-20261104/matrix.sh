# Usage: OUT=dir bash matrix.sh (from the repo root, after building build/)
cd "$(dirname "$0")/../../.."
OUT=${OUT:?set OUT}
for r in 1 2 3; do
 for e in shared unique; do
  for u in on off; do
   timeout 1200 python3 bench/s1_move.py build --units $u --email $e --json $OUT/m_${u}_${e}_$r.json > $OUT/m_${u}_${e}_$r.log 2>&1
  done
 done
done
for us in 250 4000; do for r in 1 2; do
 timeout 1200 python3 bench/s1_move.py build --units on --email shared --unit-us $us --json $OUT/sw_us${us}_$r.json > $OUT/sw_us${us}_$r.log 2>&1
done; done
for by in 65536 4194304; do for r in 1 2; do
 timeout 1200 python3 bench/s1_move.py build --units on --email unique --unit-bytes $by --json $OUT/sw_by${by}_$r.json > $OUT/sw_by${by}_$r.log 2>&1
done; done
for wb in 65536; do for r in 1 2; do
 timeout 1200 python3 bench/s1_move.py build --units on --email unique --window-bytes $wb --json $OUT/sw_wb${wb}_$r.json > $OUT/sw_wb${wb}_$r.log 2>&1
done; done
echo MATRIX DONE > $OUT/matrix.done
