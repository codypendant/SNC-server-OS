---- MODULE LiveGrantTrB6b ----
EXTENDS LiveGrant
CONSTANTS n1, n2, n3
VARIABLE stp
TS ==
 \/ stp=0 /\ WipeStop(n3)
 \/ stp=1 /\ Grant(n2, n1)
 \/ stp=2 /\ Shrink(n1, n2)
 \/ stp=3 /\ RS0(n2)
 \/ stp=4 /\ NewEntry(n1)
 \/ stp=5 /\ NewEntry(n1)
 \/ stp=6 /\ Shrink(n1, n3)
 \/ stp=7 /\ Leave(n2)
 \/ stp=8 /\ Ship(n1, n2, 1)
 \/ stp=9 /\ Ship(n1, n2, 2)
 \/ stp=10 /\ LearnS(n1)
 \/ stp=11 /\ Send(n2)
 \/ stp=12 /\ Leave(n3)
 \/ stp=13 /\ WriteLocal(n1)
 \/ stp=14 /\ ProposeGrow(n1, n2)
 \/ stp=15 /\ RS1(n2)
 \/ stp=16 /\ Deliver(n1, [t |-> 1, l |-> 2, term |-> 0, inc |-> <<0, 1>>, from |-> n2])
 \/ stp=17 /\ Drop(n1, n3)
 \/ stp=18 /\ Ack(n1, 1)
TNext == TS /\ hist' = hist \cup Held(dlog') \cup Held(ilog') /\ stp' = stp + 1
TSpec == Init /\ stp = 0 /\ [][TNext]_<<vars, stp>>
NoPast == stp <= 7
NotAt == stp # 7
EarlySafe == stp <= 7 => Safety
====
