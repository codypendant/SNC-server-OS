---- MODULE LiveGrantTrH1 ----
EXTENDS LiveGrant
CONSTANTS n1, n2, n3
VARIABLE stp
TS ==
 \/ stp=0 /\ NewEntry(n1)
 \/ stp=1 /\ NewEntry(n1)
 \/ stp=2 /\ WriteLocal(n1)
 \/ stp=3 /\ WriteLocal(n1)
 \/ stp=4 /\ Ship(n1, n2, 2)
 \/ stp=5 /\ Ship(n1, n3, 1)
 \/ stp=6 /\ Grant(n2, n1)
 \/ stp=7 /\ Grant(n3, n1)
 \/ stp=8 /\ Send(n2)
 \/ stp=9 /\ Send(n3)
 \/ stp=10 /\ \E r \in msgs : r.from = n2 /\ Deliver(n1, r)
 \/ stp=11 /\ \E r \in msgs : r.from = n3 /\ Deliver(n1, r)
 \/ stp=12 /\ Ack(n1, 1)
TNext == TS /\ hist' = hist \cup Held(dlog') \cup Held(ilog') /\ stp' = stp + 1
TSpec == Init /\ stp = 0 /\ [][TNext]_<<vars, stp>>
NoPast == stp <= 4
NotAt == stp # 4
EarlySafe == stp <= 4 => Safety
====
