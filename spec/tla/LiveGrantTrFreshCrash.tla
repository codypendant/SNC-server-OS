---- MODULE LiveGrantTrFreshCrash ----
EXTENDS LiveGrant
CONSTANTS n1, n2, n3
VARIABLE stp
TS ==
 \/ stp=0 /\ Shrink(n1, n3)
 \/ stp=1 /\ LearnS(n1)
 \/ stp=2 /\ NewEntry(n1)
 \/ stp=3 /\ WriteLocal(n1)
 \/ stp=4 /\ Ship(n1, n2, 1)
 \/ stp=5 /\ Ship(n1, n3, 1)
 \/ stp=6 /\ Grant(n2, n1)
 \/ stp=7 /\ Grant(n3, n1)
 \/ stp=8 /\ Send(n2)
 \/ stp=9 /\ Send(n3)
 \/ stp=10 /\ \E r \in msgs : r.from = n2 /\ Deliver(n1, r)
 \/ stp=11 /\ \E r \in msgs : r.from = n3 /\ Deliver(n1, r)
 \/ stp=12 /\ Ack(n1, 1)
 \/ stp=13 /\ ProposeGrow(n1, n3)
 \/ stp=14 /\ \E x \in gr[n3] : Expire(n3, x)
 \/ stp=15 /\ Replace(n3)
 \/ stp=16 /\ \E x \in gp : ChooseGrow(x)
 \/ stp=17 /\ Crash(n3)
 \/ stp=18 /\ BootRead(n3)
 \/ stp=19 /\ Promote(n3)
 \/ stp=20 /\ Learn(n3) /\ isPrim'[n3]
TNext == TS /\ hist' = hist \cup Held(dlog') \cup Held(ilog') /\ stp' = stp + 1
TSpec == Init /\ stp = 0 /\ [][TNext]_<<vars, stp>>
NoPast == stp <= 20
NotAt == stp # 20
EarlySafe == stp <= 20 => Core
====
