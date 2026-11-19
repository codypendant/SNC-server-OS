---- MODULE LiveGrantTrRead ----
EXTENDS LiveGrant
CONSTANTS n1, n2, n3
VARIABLE stp
TS ==
 \/ stp=0 /\ Shrink(n1, n3)
 \/ stp=1 /\ LearnS(n1)
 \/ stp=2 /\ Grant(n3, n1)
 \/ stp=3 /\ ProposeGrow(n1, n3)
 \/ stp=4 /\ LearnS(n1)
 \/ stp=5 /\ \E x \in gp : ChooseGrow(x)
 \/ stp=6 /\ \E x \in gr[n3] : Expire(n3, x)
 \/ stp=7 /\ Drop(n1, n3)
 \/ stp=8 /\ NewEntry(n1)
 \/ stp=9 /\ WriteLocal(n1)
 \/ stp=10 /\ Ship(n1, n2, 1)
 \/ stp=11 /\ Grant(n2, n1)
 \/ stp=12 /\ Send(n2)
 \/ stp=13 /\ \E r \in msgs : Deliver(n1, r)
 \/ stp=14 /\ Ack(n1, 1)
TNext == TS /\ hist' = hist \cup Held(dlog') \cup Held(ilog') /\ stp' = stp + 1
TSpec == Init /\ stp = 0 /\ [][TNext]_<<vars, stp>>
NoPast == stp <= 4
NotAt == stp # 4
EarlySafe == stp <= 4 => Safety
====
