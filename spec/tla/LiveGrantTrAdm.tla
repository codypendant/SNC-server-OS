---- MODULE LiveGrantTrAdm ----
EXTENDS LiveGrant
CONSTANTS n1, n2, n3
VARIABLE stp
TS ==
 \/ stp=0 /\ Shrink(n1, n2)
 \/ stp=1 /\ LearnS(n1)
 \/ stp=2 /\ RS0(n2)
 \/ stp=3 /\ Leave(n2)
 \/ stp=4 /\ ProposeGrow(n1, n2)
 \/ stp=5 /\ \E x \in gp : ChooseGrow(x)
 \/ stp=6 /\ Promote(n2)
 \/ stp=7 /\ RS1(n2)
 \/ stp=8 /\ Learn(n2)
 \/ stp=9 /\ Learn(n3)
 \/ stp=10 /\ ReqSend(n3)
 \/ stp=11 /\ \E q \in req : Answer(n2, q)
 \/ stp=12 /\ \E r \in rep[n3] : RecvRep(n3, r)
 \/ stp=13 /\ NewEntry(n2)
 \/ stp=14 /\ WriteLocal(n2)
 \/ stp=15 /\ Ship(n2, n3, 1)
 \/ stp=16 /\ Grant(n3, n2)
 \/ stp=17 /\ Send(n3)
 \/ stp=18 /\ \E r \in msgs : Deliver(n2, r)
 \/ stp=19 /\ Ack(n2, 1)
TNext == TS /\ hist' = hist \cup Held(dlog') \cup Held(ilog') /\ stp' = stp + 1
TSpec == Init /\ stp = 0 /\ [][TNext]_<<vars, stp>>
NoPast == stp <= 4
NotAt == stp # 4
EarlySafe == stp <= 4 => Safety
====
