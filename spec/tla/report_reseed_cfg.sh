#!/bin/sh
# Prints the TLC config for one ReportReseed run (docs/AFR_001_REPORT_MODEL_PREREG.md section 3):
# CA CB M1 M2 M3 M4 M5p M5r M6 M7 P1-P8, or a run name with an f suffix for its fallback.
set -eu
run=$1
case $run in CA|CB|M1|M2|M3|M4|M5p|M5r|M6|M7|P[1-8]|CAf|CBf|M1f|M2f|M3f|M4f|M5pf|M5rf|M6f|M7f) ;; *) echo "unknown run $run" >&2; exit 1 ;; esac
case $run in *f) fb=1; base=${run%f} ;; *) fb=0; base=$run ;; esac
case $base in CB|M2|M5p|P7|P8) lane=B ;; *) lane=A ;; esac
fix() { [ "$base" = "$1" ] && echo FALSE || echo TRUE; }
if [ $lane = A ]; then
  nodes='{n1, n2, n3}'; term=2; crashes=1; msgs=2; reseeds=1; wipes=1
else
  nodes='{n1, n2, n3, n4}'; term=3; crashes=1; msgs=1; reseeds=0; wipes=0
fi
if [ $fb = 1 ]; then
  if [ $lane = B ]; then crashes=0; else msgs=1; fi
fi
cat <<CFG
CONSTANTS
  Nodes = $nodes
  P0 = n1
  MaxTerm = $term
  MaxIds = 2
  MaxCrashes = $crashes
  MaxReps = 2
  MaxMsgs = $msgs
  MaxReseeds = $reseeds
  MaxWipes = $wipes
  FixInc = $(fix M1)
  FixTerm = $(fix M2)
  FixWait = $(fix M3)
  FixEntry = $(fix M4)
  FixSendP = $(fix M5p)
  FixSendR = $(fix M5r)
  FixNonce = $(fix M6)
  FixGrow = $(fix M7)
SPECIFICATION Spec
CFG
[ $lane = B ] && echo "SYMMETRY Sym"
case $base in
  P1) echo "INVARIANT NoRejectByInc" ;;
  P2) echo "INVARIANT NoRejectByTerm" ;;
  P3) echo "INVARIANT NoRejectByEntry" ;;
  P4) echo "INVARIANT NoShortReseed" ;;
  P5) echo "INVARIANT NoWipe" ;;
  P6) echo "INVARIANT NoCrashInReseed" ;;
  P7) echo "INVARIANT NoRejectByTerm" ;;
  P8) echo "INVARIANT NoAckAfterR7" ;;
  *) echo "INVARIANT Safety" ;;
esac
