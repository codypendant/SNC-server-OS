---- MODULE LiveGrantTrOpLeave ----
EXTENDS LiveGrant
CONSTANTS n1, n2, n3
VARIABLE stp
TS ==
 \/ stp=0 /\ Shrink(n1, n3)
 \/ stp=1 /\ Shrink(n1, n2)
 \/ stp=2 /\ RS0(n3)
 \/ stp=3 /\ Leave(n3)
 \/ stp=4 /\ OpOutside(n3)
 \/ stp=5 /\ RS1(n3)
 \/ stp=6 /\ Learn(n3) /\ isPrim'[n3]
TNext == TS /\ hist' = hist \cup Held(dlog') \cup Held(ilog') /\ stp' = stp + 1
TSpec == Init /\ stp = 0 /\ [][TNext]_<<vars, stp>>
NoPast == stp <= 6
NotAt == stp # 6
EarlySafe == stp <= 6 => Safety
====
