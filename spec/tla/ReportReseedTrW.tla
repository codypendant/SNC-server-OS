---- MODULE ReportReseedTrW ----
EXTENDS ReportReseed
CONSTANTS n1, n2, n3
VARIABLE stp
TS ==
 \/ stp=0 /\ Promote(n2)
 \/ stp=1 /\ Learn(n2)
 \/ stp=2 /\ Learn(n3)
 \/ stp=3 /\ ReconStep(n3,n2)
 \/ stp=4 /\ \E r \in rep[n3] : RecvRep(n3, r)
 \/ stp=5 /\ NewEntry(n2)
 \/ stp=6 /\ WriteLocal(n2)
 \/ stp=7 /\ Ship(n2,n3)
 \/ stp=8 /\ Send(n3)
 \/ stp=9 /\ \E r \in msgs : Deliver(n2, r)
 \/ stp=10 /\ Shrink(n2,n3)
 \/ stp=11 /\ Wipe(n3,n1,0)
 \/ stp=12 /\ Learn(n3)
 \/ stp=13 /\ BootRead(n3)
 \/ stp=14 /\ Grant(n3,n2)
 \/ stp=15 /\ Ack(n2,1)
TNext == TS /\ hist' = hist \cup Held(dlog') \cup Held(ilog') /\ stp' = stp + 1
TSpec == Init /\ stp = 0 /\ [][TNext]_<<vars, stp>>
====
