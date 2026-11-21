#!/bin/sh
# Prints the TLC config for one LiveGrant run (docs/AFR_001_LIVE_GRANT_PREREG.md section 7).
# RUN is a run name; a trailing "d" or "e" adds the depth-bounded fallback (CONSTRAINT Shallow or Shallow16).
set -eu
run=$1
case $run in *d) deep=Shallow; base=${run%d} ;; *e) deep=Shallow16; base=${run%e} ;; *) deep=; base=$run ;; esac
case $base in
  C-LT|C-LS|C-LW|C-LC|C-LR|C-LI|C-LB|PR|N1|N2B|N3|N4|N5|N6|N7|N8p|N8pB|N8r|N9|N10a|N11|N11B|N12|N13|N14|N15|N16|N17|N18|N19|N20|P[1-9]|P10) ;;
  *) echo "unknown run $run" >&2; exit 1 ;;
esac
case $base in
  C-LT|N3|N7|N8p|N11|N16|N17|P4|P8) lane=LT ;;
  C-LS|N4|N6|N8r|N10a|N12|N13|N14|N18|P3|P5) lane=LS ;;
  C-LW|N1|N5|N9|P1|P2|P6) lane=LW ;;
  C-LC|N15) lane=LC ;;
  C-LR|PR|N19|P10) lane=LR ;;
  N20) lane=LRC ;;
  P7) lane=LSC ;;
  C-LI) lane=LI ;;
  *) lane=LB ;;
esac
off=
case $base in
  N1) off=FixGrantInc ;; N2B) off=FixGenI ;; N3) off=FixWaitT ;; N4) off=FixWaitR ;;
  N5) off=FixWaitW ;; N6) off=FixLG5 ;; N7) off=FixEntry ;; N8p|N8pB) off=FixSendP ;; N8r) off=FixSendR ;;
  N9) off=FixNonce ;; N10a) off=FixCovGrow ;; N12) off=FixStop ;; N13) off=FixGenR ;;
  N14) off=FixOrder ;; N15) off=FixGenB ;; N16) off=FixCont ;; N17) off=FixRead ;; N18) off=FixAdmGrant ;;
  N19) off=FixRefuse ;; N20) off=FixFreshDur ;;
esac
addterm=FALSE; case $base in N11|N11B) addterm=TRUE ;; esac
nodes='{n1, n2, n3}'; term=2; crashes=0; reseeds=0; wipes=0; repl=FALSE; opin=FALSE
case $lane in
  LS) reseeds=1 ;; LW) wipes=1 ;; LC) crashes=1 ;; LR) repl=TRUE; wipes=1 ;;
  LRC) repl=TRUE; wipes=1; crashes=1 ;; LSC) reseeds=1; crashes=1 ;; LI) opin=TRUE ;;
  LB) nodes='{n1, n2, n3, n4}'; term=3 ;;
esac
flag() { for f in $off; do [ "$f" = "$1" ] && { echo FALSE; return; }; done; echo TRUE; }
cat <<CFG
CONSTANTS
  Nodes = $nodes
  P0 = n1
  MaxTerm = $term
  MaxIds = 1
  MaxCrashes = $crashes
  MaxReps = 1
  MaxMsgs = 1
  MaxGot = 1
  MaxReqs = 1
  MaxReseeds = $reseeds
  MaxWipes = $wipes
  MaxVer = 4
  Repl = $repl
  OpInside = $opin
  RecAct = FALSE
  AddTerm = $addterm
CFG
for f in FixGrantInc FixGenI FixWaitT FixWaitR FixWaitW FixLG5 FixEntry FixSendP FixSendR FixNonce \
         FixCovGrow FixStop FixGenR FixOrder FixGenB FixCont FixRead FixAdmGrant FixRefuse FixFreshDur; do
  echo "  $f = $(flag $f)"
done
echo "SPECIFICATION Spec"
[ $lane = LB ] && echo "SYMMETRY Sym"
[ -n "$deep" ] && echo "CONSTRAINT $deep"
case $base in
  P1) echo "INVARIANT NoRejectByInc" ;;
  P2) echo "INVARIANT NoLateRequest" ;;
  P3) echo "INVARIANT NoBumpKill" ;;
  P4) echo "INVARIANT NoWaitBlock" ;;
  P5) echo "INVARIANT NoShortReseed" ;;
  P6) echo "INVARIANT NoWipe" ;;
  P7) echo "INVARIANT NoCrashInReseed" ;;
  P8|P9) echo "INVARIANT NoAckAfterR7" ;;
  P10) echo "INVARIANT NoReplace" ;;
  PR) echo "INVARIANT GrowSound" ;;
  C-LR|N19|N20) for i in AckSound LeaseSound PromoSound OutsideS EntryUnique LogMatching HistoryUnique; do echo "INVARIANT $i"; done ;;
  *) for i in AckSound LeaseSound PromoSound OutsideS EntryUnique LogMatching HistoryUnique GrowSound AckedPreserved; do echo "INVARIANT $i"; done ;;
esac
exit 0
