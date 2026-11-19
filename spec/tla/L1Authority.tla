---------------------------- MODULE L1Authority ----------------------------
(* L-1: exclusive authority across a degrading shrink and R7 case 1.      *)
(* Authority part of Combined.tla, with the shrink split into a request   *)
(* that may be chosen late and a reply that may be lost.                  *)
EXTENDS Integers, FiniteSets

CONSTANTS Nodes, P0, Q0, D, MaxTerm, MaxVer, MaxRestarts,
          FixA1,      \* degraded authority required from the send of a degrading shrink
          FixVerCAS,  \* a shrink applies only to the version it was proposed on
          FixDecide,  \* p leaves the pending state only once a later version is chosen
          OpInside, OpOutsideDeg, R2p

VARIABLES rec, T, isPrim, learnW, A, pdeg, hg, wl, pend, prop, nRestarts, l1

vars == <<rec, T, isPrim, learnW, A, pdeg, hg, wl, pend, prop, nRestarts, l1>>

Dec(x) == IF x > 0 THEN x - 1 ELSE 0
NoGrants == [m \in Nodes |-> 0]
None == [ver |-> -1, term |-> 0, c |-> P0]

Init ==
  /\ rec = [ver |-> 0, term |-> 1, prim |-> P0, S |-> {P0, Q0}, ready |-> TRUE, deg |-> FALSE]
  /\ T = [n \in Nodes |-> 1]
  /\ isPrim = [n \in Nodes |-> n = P0]
  /\ learnW = [n \in Nodes |-> 0]
  /\ A = [n \in Nodes |-> IF n = P0 THEN {P0, Q0} ELSE {}]
  /\ pdeg = [n \in Nodes |-> FALSE]
  /\ hg = [n \in Nodes |-> IF n = P0 THEN [m \in Nodes |-> IF m = Q0 THEN D ELSE 0] ELSE NoGrants]
  /\ wl = [n \in Nodes |-> 0]
  /\ pend = [n \in Nodes |-> FALSE]
  /\ prop = [n \in Nodes |-> None]
  /\ nRestarts = 0
  /\ l1 = FALSE

Auth(n) == /\ isPrim[n] /\ learnW[n] = 0
           /\ \A m \in A[n] \ {n} : hg[n][m] > 0
           /\ (pdeg[n] => wl[n] > 0)
Current(p) == isPrim[p] /\ rec.prim = p /\ rec.term = T[p]
Chg(r) == rec.ver < MaxVer /\ rec' = [r EXCEPT !.ver = rec.ver + 1]

Tick ==
  /\ learnW' = [n \in Nodes |-> Dec(learnW[n])]
  /\ hg' = [n \in Nodes |-> [m \in Nodes |-> Dec(hg[n][m])]]
  /\ wl' = [n \in Nodes |-> Dec(wl[n])]
  /\ UNCHANGED <<rec, T, isPrim, A, pdeg, pend, prop, nRestarts, l1>>

Grant(m, n) ==
  /\ m # n /\ isPrim[n] /\ T[m] = T[n] /\ m \in A[n]
  /\ hg' = [hg EXCEPT ![n][m] = D]
  /\ UNCHANGED <<rec, T, isPrim, learnW, A, pdeg, wl, pend, prop, nRestarts, l1>>

WGrant(n) ==
  /\ isPrim[n] /\ pdeg[n] /\ rec.term = T[n] /\ rec.prim = n /\ rec.deg
  /\ wl' = [wl EXCEPT ![n] = D]
  /\ UNCHANGED <<rec, T, isPrim, learnW, A, pdeg, hg, pend, prop, nRestarts, l1>>

NewS(j) == IF R2p /\ j \in rec.S THEN rec.S \ {rec.prim} ELSE {j}

Promote(j) ==
  /\ rec.term < MaxTerm
  /\ \/ j # rec.prim /\ j \in rec.S /\ (rec.ready \/ OpInside)
     \/ j # rec.prim /\ OpOutsideDeg /\ j \notin rec.S /\ rec.deg /\ rec.ready
     \/ R2p /\ j = rec.prim /\ rec.S = {j} /\ rec.ready /\ ~isPrim[j] /\ T[j] = rec.term
  /\ Chg([rec EXCEPT !.term = rec.term + 1, !.prim = j, !.S = NewS(j), !.ready = FALSE,
                     !.deg = (NewS(j) = {j})])
  /\ l1' = (l1 \/ (j \notin rec.S /\ isPrim[rec.prim] /\ T[rec.prim] = rec.term
                    /\ \E m \in A[rec.prim] \ {rec.prim} : hg[rec.prim][m] > 0))
  /\ UNCHANGED <<T, isPrim, learnW, A, pdeg, hg, wl, pend, prop, nRestarts>>

Learn(n) ==
  /\ rec.term > T[n]
  /\ T' = [T EXCEPT ![n] = rec.term]
  /\ isPrim' = [isPrim EXCEPT ![n] = rec.prim = n]
  /\ learnW' = [learnW EXCEPT ![n] = IF rec.prim = n THEN D ELSE @]
  /\ A' = [A EXCEPT ![n] = IF rec.prim = n THEN (IF R2p THEN rec.S ELSE {n}) ELSE {}]
  /\ pdeg' = [pdeg EXCEPT ![n] = (rec.prim = n) /\ (R2p => rec.deg)]
  /\ hg' = [hg EXCEPT ![n] = NoGrants]
  /\ wl' = [wl EXCEPT ![n] = 0]
  /\ pend' = [pend EXCEPT ![n] = FALSE]
  /\ UNCHANGED <<rec, prop, nRestarts, l1>>

Ready(p) ==
  /\ Current(p) /\ ~pend[p] /\ ~rec.ready /\ learnW[p] = 0
  /\ Chg([rec EXCEPT !.ready = TRUE])
  /\ UNCHANGED <<T, isPrim, learnW, A, pdeg, hg, wl, pend, prop, nRestarts, l1>>

Restart(n) ==
  /\ nRestarts < MaxRestarts
  /\ nRestarts' = nRestarts + 1
  /\ isPrim' = [isPrim EXCEPT ![n] = FALSE]
  /\ A' = [A EXCEPT ![n] = {}]
  /\ pdeg' = [pdeg EXCEPT ![n] = FALSE]
  /\ hg' = [hg EXCEPT ![n] = NoGrants]
  /\ wl' = [wl EXCEPT ![n] = 0]
  /\ pend' = [pend EXCEPT ![n] = FALSE]
  /\ UNCHANGED <<rec, T, learnW, prop, l1>>

AddA(p, c) ==
  /\ Current(p) /\ c # p /\ c \notin A[p] /\ T[c] = T[p]
  /\ A' = [A EXCEPT ![p] = @ \cup {c}]
  /\ UNCHANGED <<rec, T, isPrim, learnW, pdeg, hg, wl, pend, prop, nRestarts, l1>>

Grow(p, c) ==
  /\ Current(p) /\ ~pend[p] /\ c \in A[p] /\ c \notin rec.S /\ T[c] = T[p]
  /\ Chg([rec EXCEPT !.S = @ \cup {c}, !.deg = FALSE])
  /\ pdeg' = [pdeg EXCEPT ![p] = FALSE]
  /\ UNCHANGED <<T, isPrim, learnW, A, hg, wl, pend, prop, nRestarts, l1>>

ShrinkSend(p, c) ==
  /\ Current(p) /\ ~pend[p] /\ c \in rec.S /\ c # p
  /\ prop' = [prop EXCEPT ![p] = [ver |-> rec.ver, term |-> rec.term, c |-> c]]
  /\ pend' = [pend EXCEPT ![p] = TRUE]
  /\ pdeg' = [pdeg EXCEPT ![p] = @ \/ (FixA1 /\ rec.S \ {c} = {p})]
  /\ UNCHANGED <<rec, T, isPrim, learnW, A, hg, wl, nRestarts, l1>>

ShrinkChosen(p) ==
  /\ prop[p] # None /\ rec.prim = p /\ rec.term = prop[p].term /\ prop[p].c \in rec.S
  /\ FixVerCAS => rec.ver = prop[p].ver
  /\ Chg([rec EXCEPT !.S = @ \ {prop[p].c}, !.deg = (rec.S \ {prop[p].c} = {p})])
  /\ prop' = [prop EXCEPT ![p] = None]
  /\ UNCHANGED <<T, isPrim, learnW, A, pdeg, hg, wl, pend, nRestarts, l1>>

Bump(p) ==
  /\ Current(p) /\ pend[p] /\ rec.ver = prop[p].ver
  /\ Chg(rec)
  /\ UNCHANGED <<T, isPrim, learnW, A, pdeg, hg, wl, pend, prop, nRestarts, l1>>

LearnOut(p) ==
  /\ Current(p) /\ pend[p] /\ (FixDecide => rec.ver > prop[p].ver)
  /\ pend' = [pend EXCEPT ![p] = FALSE]
  /\ pdeg' = [pdeg EXCEPT ![p] = rec.deg]
  /\ UNCHANGED <<rec, T, isPrim, learnW, A, hg, wl, prop, nRestarts, l1>>

Drop(p, c) ==
  /\ isPrim[p] /\ c \in A[p] /\ c # p /\ c \notin rec.S /\ rec.term = T[p]
  /\ A' = [A EXCEPT ![p] = @ \ {c}]
  /\ pdeg' = [pdeg EXCEPT ![p] = @ \/ rec.deg]
  /\ UNCHANGED <<rec, T, isPrim, learnW, hg, wl, pend, prop, nRestarts, l1>>

Next ==
  \/ Tick
  \/ \E n \in Nodes : Promote(n) \/ Learn(n) \/ Ready(n) \/ Restart(n) \/ WGrant(n)
                      \/ ShrinkChosen(n) \/ Bump(n) \/ LearnOut(n)
  \/ \E m, n \in Nodes : Grant(m, n) \/ AddA(m, n) \/ Grow(m, n) \/ ShrinkSend(m, n) \/ Drop(m, n)

Spec == Init /\ [][Next]_vars

OneAuthority == \A a, b \in Nodes : a # b => ~(Auth(a) /\ Auth(b))
NoL1Window == ~l1
=============================================================================
