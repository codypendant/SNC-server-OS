#!/bin/sh
# Regression traces for LiveGrant (docs/AFR_001_LIVE_GRANT_PREREG.md section 6). For each trace:
# correct model: property P holds and the trace is refused at its break step (NoPast holds, NotAt violated);
# matching mutation: P violated, and not before the break step (EarlySafe holds). P is Safety unless given.
set -u
J=${TLA2TOOLS:?set TLA2TOOLS to tla2tools.jar}
cd "$(dirname "$0")"
tlc() { java -cp "$J" tlc2.TLC -deadlock -workers 1 -config "$2" "$1.tla" 2>&1 | grep -q "$3" && echo ok || echo FAIL; }
check() {
  t=$1; shift; P=Safety; [ "$1" = Core ] && { P=Core; shift; }
  inv() { sed '/^INVARIANT/d' $t.cfg > $t.x.cfg; for i in "$@"; do echo "INVARIANT $i" >> $t.x.cfg; done; }
  mut() { for f in $flags; do sed -i "s/  $f = TRUE/  $f = FALSE/" $t.x.cfg; done; }
  flags="$*"
  a=$(tlc $t $t.cfg "No error")
  inv NotAt; b=$(tlc $t $t.x.cfg "is violated")
  inv $P; mut; c=$(tlc $t $t.x.cfg "is violated")
  inv EarlySafe; mut; d=$(tlc $t $t.x.cfg "No error")
  rm -f $t.x.cfg ${t}_TTrace_*; rm -rf states
  echo "$t: correct refused $a, reaches break $b, mutation violates $c, not before break $d"
}
check LiveGrantTrR7 FixWaitT
check LiveGrantTrW FixGrantInc
check LiveGrantTrF1b FixLG5
check LiveGrantTrF1a FixCovGrow
check LiveGrantTrH1 FixCont
check LiveGrantTrKIP903 FixLG5
check LiveGrantTrRead FixRead
check LiveGrantTrAdm FixAdmGrant
check LiveGrantTrB6b FixWaitR
check LiveGrantTrFresh Core FixRefuse
check LiveGrantTrFreshCrash Core FixFreshDur
check LiveGrantTrOpLeave FixRefuse
