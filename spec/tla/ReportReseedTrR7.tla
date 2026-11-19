---- MODULE ReportReseedTrR7 ----
EXTENDS ReportReseed
CONSTANTS n1, n2, n3
VARIABLE stp
TS ==
 \/ stp=0 /\ Shrink(n1,n3)
 \/ stp=1 /\ LearnS(n1)
 \/ stp=2 /\ Drop(n1,n3)
 \/ stp=3 /\ Shrink(n1,n2)
 \/ stp=4 /\ OpOutside(n3)
 \/ stp=5 /\ NewEntry(n1)
 \/ stp=6 /\ NewEntry(n1)
 \/ stp=7 /\ WriteLocal(n1)
 \/ stp=8 /\ WriteLocal(n1)
 \/ stp=9 /\ Ship(n1,n3)
 \/ stp=10 /\ Ship(n1,n2)
 \/ stp=11 /\ Ship(n1,n2)
 \/ stp=12 /\ Grant(n2,n1)
 \/ stp=13 /\ Send(n2)
 \/ stp=14 /\ \E r \in msgs : Deliver(n1, r)
 \/ stp=15 /\ Learn(n3)
 \/ stp=16 /\ Learn(n2)
 \/ stp=17 /\ ReconStep(n2,n3)
 \/ stp=18 /\ \E r \in rep[n2] : RecvRep(n2, r)
 \/ stp=19 /\ ApplyIntent(n2)
 \/ stp=20 /\ Ack(n1,2)
TNext == TS /\ hist' = hist \cup Held(dlog') \cup Held(ilog') /\ stp' = stp + 1
TSpec == Init /\ stp = 0 /\ [][TNext]_<<vars, stp>>
====
