---- MODULE LiveGrantTrW ----
EXTENDS LiveGrant
CONSTANTS n1, n2, n3
VARIABLE stp
TS ==
 \/ stp=0 /\ Promote(n2)
 \/ stp=1 /\ Learn(n2)
 \/ stp=2 /\ Learn(n3)
 \/ stp=3 /\ ReqSend(n3)
 \/ stp=4 /\ \E q \in req : Answer(n2, q)
 \/ stp=5 /\ \E r \in rep[n3] : RecvRep(n3, r)
 \/ stp=6 /\ NewEntry(n2)
 \/ stp=7 /\ WriteLocal(n2)
 \/ stp=8 /\ Ship(n2, n3, 1)
 \/ stp=9 /\ Grant(n3, n2)
 \/ stp=10 /\ Send(n3)
 \/ stp=11 /\ \E r \in msgs : Deliver(n2, r)
 \/ stp=12 /\ Shrink(n2, n3)
 \/ stp=13 /\ WipeStop(n3)
 \/ stp=14 /\ \E x \in gr[n3] : Expire(n3, x)
 \/ stp=15 /\ Leave(n3)
 \/ stp=16 /\ WipeDo(n3, n1, 0)
 \/ stp=17 /\ Learn(n3)
 \/ stp=18 /\ BootRead(n3)
 \/ stp=19 /\ ReqSend(n3)
 \/ stp=20 /\ Answer(n2, [from |-> n3, inc |-> <<1, 1>>, term |-> 2, blt |-> 0, ble |-> 0])
 \/ stp=21 /\ \E r \in rep[n3] : RecvRep(n3, r)
 \/ stp=22 /\ Answer(n2, [from |-> n3, inc |-> <<0, 1>>, term |-> 2, blt |-> 0, ble |-> 0])
 \/ stp=23 /\ Grant(n3, n2)
 \/ stp=24 /\ Ack(n2, 1)
TNext == TS /\ hist' = hist \cup Held(dlog') \cup Held(ilog') /\ stp' = stp + 1
TSpec == Init /\ stp = 0 /\ [][TNext]_<<vars, stp>>
NoPast == stp <= 24
NotAt == stp # 24
EarlySafe == stp <= 24 => Safety
====
