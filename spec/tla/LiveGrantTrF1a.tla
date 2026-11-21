---- MODULE LiveGrantTrF1a ----
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
 \/ stp=13 /\ RS0(n3)
 \/ stp=14 /\ \E x \in gr[n3] : Expire(n3, x)
 \/ stp=15 /\ Leave(n3)
 \/ stp=16 /\ RS1(n3)
 \/ stp=17 /\ RS2(n3)
 \/ stp=18 /\ RS3(n3, n1)
 \/ stp=19 /\ RS5(n3, 0)
 \/ stp=20 /\ RS6(n3)
 \/ stp=21 /\ ReqSend(n3)
 \/ stp=22 /\ \E q \in req : Answer(n1, q)
 \/ stp=23 /\ \E r \in rep[n3] : RecvRep(n3, r)
 \/ stp=24 /\ Grant(n3, n1)
 \/ stp=25 /\ ProposeGrow(n1, n3)
 \/ stp=26 /\ \E x \in gp : ChooseGrow(x)
TNext == TS /\ hist' = hist \cup Held(dlog') \cup Held(ilog') /\ stp' = stp + 1
TSpec == Init /\ stp = 0 /\ [][TNext]_<<vars, stp>>
NoPast == stp <= 25
NotAt == stp # 25
EarlySafe == stp <= 25 => Safety
====
