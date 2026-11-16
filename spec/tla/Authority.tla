---------------------------- MODULE Authority ----------------------------
(***************************************************************************)
(* SNC-AFR-001 Claim L: at most one node holds valid authority.            *)
(* Verification artifact for docs/AFR_001_SAFETY_ANALYSIS.md, R1-R9.       *)
(* Not product code.                                                       *)
(*                                                                         *)
(* Time is one global integer `now`, counted in real time. Drift inside    *)
(* the stated bound is abstracted away as follows (section 0 of the        *)
(* analysis):                                                              *)
(*   - a grant made at time g supports its holder while now < g + D;       *)
(*   - a wait of W on a waiter's clock lasts at least D, so a waiter that  *)
(*     started at s may act once now >= s + D.                             *)
(* Drift beyond the bound is out of contract and is not modelled.          *)
(*                                                                         *)
(* The group record is a single register; witness Paxos is abstracted.    *)
(*   - A promotion is accepted witness by witness, and is chosen at a      *)
(*     majority. Its proposer is modelled as never failing.                *)
(*   - Other compare-and-sets (grow, shrink, ready) are atomic, and are    *)
(*     disabled while a promotion is pending. This stands in for           *)
(*     compare-and-set exclusion, which the C++ tests already cover.       *)
(*                                                                         *)
(* Each Fix* constant switches one fix on. Turning a fix off must let TLC  *)
(* find the failure that fix prevents.                                     *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS Nodes, Witnesses, P0, Q0, D, MaxTime, MaxTerm, MaxRestarts, MaxFences,
          FixR3,      \* R3: eligibility counts grants made since boot
          Barrier,    \* R3/R8: wait D after learning the promotion was chosen
          FixReady,   \* R2: READY only after the primary is eligible (B2)
          W1,         \* R8: a witness grants only per its accepted record
          FixM2,      \* R9: a standby lease never outlasts the primary's
          FixOp,      \* R7: outside-S from degraded needs ready or fenced
          OpInside,   \* allow operator promotion inside S without READY
          AllowOp5c,  \* allow outside-S promotion with |S|>=2 unfenced (5c)
          OpOutside,  \* allow operator promotion outside S at all
          Degrade,    \* allow the degrade step (Shrink)
          Standby     \* allow standby read leases

NONE == -10

VARIABLES now, rec, propOn, prop, acc, wrec,
          T, isPrim, fenced, boot, lastGrant, restarted, learned,
          A, pdeg, hg, hw, sl, nRestarts

vars == <<now, rec, propOn, prop, acc, wrec, T, isPrim, fenced, boot,
          lastGrant, restarted, learned, A, pdeg, hg, hw, sl, nRestarts>>

Maj(Q) == 2 * Cardinality(Q) > Cardinality(Witnesses)
Min(S) == CHOOSE x \in S : \A y \in S : x <= y
Max(S) == CHOOSE x \in S : \A y \in S : x >= y
NoGrants == [m \in Nodes |-> NONE]
NoWGrants == [w \in Witnesses |-> NONE]

Init ==
  /\ now = 0
  /\ rec = [term |-> 1, prim |-> P0, S |-> {P0, Q0}, ready |-> TRUE, deg |-> FALSE]
  /\ propOn = FALSE
  /\ prop = rec
  /\ acc = {}
  /\ wrec = [w \in Witnesses |-> rec]
  /\ T = [n \in Nodes |-> 1]
  /\ isPrim = [n \in Nodes |-> n = P0]
  /\ fenced = [n \in Nodes |-> FALSE]
  /\ boot = [n \in Nodes |-> -D]
  /\ lastGrant = [n \in Nodes |-> IF n = Q0 THEN 0 ELSE NONE]
  /\ restarted = [n \in Nodes |-> FALSE]
  /\ learned = [n \in Nodes |-> -D]
  /\ A = [n \in Nodes |-> IF n = P0 THEN {P0, Q0} ELSE {}]
  /\ pdeg = [n \in Nodes |-> FALSE]
  /\ hg = [n \in Nodes |-> IF n = P0 THEN [NoGrants EXCEPT ![Q0] = 0] ELSE NoGrants]
  /\ hw = [n \in Nodes |-> NoWGrants]
  /\ sl = [n \in Nodes |-> [term |-> 0, exp |-> NONE]]
  /\ nRestarts = 0

(* --- authority -------------------------------------------------------- *)
Eligible(n) ==
  /\ isPrim[n] /\ ~fenced[n]
  /\ now >= boot[n] + D
  /\ now >= lastGrant[n] + D
  /\ (Barrier => now >= learned[n] + D)

MemberLease(n) == \A m \in A[n] \ {n} : now < hg[n][m] + D
WitnessLease(n) == \E Q \in SUBSET Witnesses :
                     Maj(Q) /\ \A w \in Q : now < hw[n][w] + D
Auth(n) == Eligible(n) /\ MemberLease(n) /\ (pdeg[n] => WitnessLease(n))

MEnd(n) == IF A[n] \ {n} = {} THEN MaxTime + D + 1
           ELSE Min({hg[n][m] + D : m \in A[n] \ {n}})
WEnd(n) == Max({e \in {hw[n][w] + D : w \in Witnesses} :
                 Maj({w \in Witnesses : hw[n][w] + D >= e})})
End(n) == IF pdeg[n] THEN Min({MEnd(n), WEnd(n)}) ELSE MEnd(n)

StandbyValid(m) == ~fenced[m] /\ sl[m].term = T[m] /\ now < sl[m].exp

Current(p) == isPrim[p] /\ ~fenced[p] /\ rec.prim = p /\ rec.term = T[p]

(* --- actions ---------------------------------------------------------- *)
Tick == now < MaxTime /\ now' = now + 1
        /\ UNCHANGED <<rec, propOn, prop, acc, wrec, T, isPrim, fenced, boot,
                       lastGrant, restarted, learned, A, pdeg, hg, hw, sl, nRestarts>>

\* member m grants to the primary n of m's own known term
Grant(m, n) ==
  /\ m # n /\ ~fenced[m] /\ isPrim[n] /\ ~fenced[n] /\ T[m] = T[n] /\ m \in A[n]
  /\ hg' = [hg EXCEPT ![n][m] = now]
  /\ lastGrant' = [lastGrant EXCEPT ![m] = IF FixR3 \/ ~restarted[m] THEN now ELSE @]
  /\ UNCHANGED <<now, rec, propOn, prop, acc, wrec, T, isPrim, fenced, boot,
                 restarted, learned, A, pdeg, hw, sl, nRestarts>>

WGrant(w, n) ==
  /\ isPrim[n] /\ ~fenced[n] /\ pdeg[n]
  /\ (W1 => (wrec[w].term = T[n] /\ wrec[w].prim = n /\ wrec[w].deg))
  /\ hw' = [hw EXCEPT ![n][w] = now]
  /\ UNCHANGED <<now, rec, propOn, prop, acc, wrec, T, isPrim, fenced, boot,
                 lastGrant, restarted, learned, A, pdeg, hg, sl, nRestarts>>

AllSFenced == \A m \in rec.S : fenced[m]
MayPromote(j) ==
  /\ j # rec.prim /\ ~fenced[j]
  /\ \/ (j \in rec.S /\ rec.ready)                       \* automatic (R2)
     \/ (j \in rec.S /\ OpInside)                        \* operator inside S (R7)
     \/ (OpOutside /\ j \notin rec.S /\ rec.deg
           /\ (FixOp => (rec.ready \/ AllSFenced)))      \* operator outside S, degraded
     \/ (OpOutside /\ j \notin rec.S /\ ~rec.deg /\ (AllowOp5c \/ AllSFenced))

Propose(j) ==
  /\ ~propOn /\ rec.term < MaxTerm /\ MayPromote(j)
  /\ propOn' = TRUE /\ acc' = {}
  /\ prop' = [term |-> rec.term + 1, prim |-> j, S |-> {j}, ready |-> FALSE, deg |-> TRUE]
  /\ UNCHANGED <<now, rec, wrec, T, isPrim, fenced, boot, lastGrant, restarted,
                 learned, A, pdeg, hg, hw, sl, nRestarts>>

WAccept(w) ==
  /\ propOn /\ w \notin acc
  /\ wrec' = [wrec EXCEPT ![w] = prop]
  /\ acc' = acc \cup {w}
  /\ IF Maj(acc \cup {w})
       THEN rec' = prop /\ propOn' = FALSE
       ELSE UNCHANGED <<rec, propOn>>
  /\ UNCHANGED <<now, prop, T, isPrim, fenced, boot, lastGrant, restarted, learned,
                 A, pdeg, hg, hw, sl, nRestarts>>

\* atomic non-term compare-and-set by the current primary
SetRec(r) == rec' = r /\ wrec' = [w \in Witnesses |-> r]

AddA(p, c) ==
  /\ Current(p) /\ ~propOn /\ c # p /\ c \notin A[p] /\ ~fenced[c]
  /\ A' = [A EXCEPT ![p] = @ \cup {c}]
  /\ UNCHANGED <<now, rec, propOn, prop, acc, wrec, T, isPrim, fenced, boot,
                 lastGrant, restarted, learned, pdeg, hg, hw, sl, nRestarts>>

Grow(p, c) ==
  /\ Current(p) /\ ~propOn /\ c \in A[p] /\ c \notin rec.S
  /\ SetRec([rec EXCEPT !.S = @ \cup {c}, !.deg = FALSE])
  /\ pdeg' = [pdeg EXCEPT ![p] = FALSE]
  /\ UNCHANGED <<now, propOn, prop, acc, T, isPrim, fenced, boot, lastGrant,
                 restarted, learned, A, hg, hw, sl, nRestarts>>

Shrink(p) ==   \* degrade: S becomes {p}; R1.2 lets p drop the others from A
  /\ Degrade /\ Current(p) /\ ~propOn /\ rec.S # {p}
  /\ SetRec([rec EXCEPT !.S = {p}, !.deg = TRUE])
  /\ pdeg' = [pdeg EXCEPT ![p] = TRUE]
  /\ A' = [A EXCEPT ![p] = {p}]
  /\ UNCHANGED <<now, propOn, prop, acc, T, isPrim, fenced, boot, lastGrant,
                 restarted, learned, hg, hw, sl, nRestarts>>

Ready(p) ==
  /\ Current(p) /\ ~propOn /\ ~rec.ready
  /\ (FixReady => Eligible(p))
  /\ SetRec([rec EXCEPT !.ready = TRUE])
  /\ UNCHANGED <<now, propOn, prop, acc, T, isPrim, fenced, boot, lastGrant,
                 restarted, learned, A, pdeg, hg, hw, sl, nRestarts>>

Learn(n) ==
  /\ ~fenced[n] /\ rec.term > T[n]
  /\ T' = [T EXCEPT ![n] = rec.term]
  /\ isPrim' = [isPrim EXCEPT ![n] = (rec.prim = n)]
  /\ learned' = [learned EXCEPT ![n] = IF rec.prim = n THEN now ELSE @]
  /\ A' = [A EXCEPT ![n] = IF rec.prim = n THEN {n} ELSE {}]
  /\ pdeg' = [pdeg EXCEPT ![n] = (rec.prim = n)]
  /\ hg' = [hg EXCEPT ![n] = NoGrants]
  /\ hw' = [hw EXCEPT ![n] = NoWGrants]
  /\ UNCHANGED <<now, rec, propOn, prop, acc, wrec, fenced, boot, lastGrant,
                 restarted, sl, nRestarts>>

Restart(n) ==
  /\ ~fenced[n] /\ nRestarts < MaxRestarts
  /\ nRestarts' = nRestarts + 1
  /\ boot' = [boot EXCEPT ![n] = now]
  /\ lastGrant' = [lastGrant EXCEPT ![n] = NONE]
  /\ restarted' = [restarted EXCEPT ![n] = TRUE]
  /\ isPrim' = [isPrim EXCEPT ![n] = FALSE]          \* R4 / R6.0
  /\ A' = [A EXCEPT ![n] = {}]
  /\ pdeg' = [pdeg EXCEPT ![n] = FALSE]
  /\ hg' = [hg EXCEPT ![n] = NoGrants]
  /\ hw' = [hw EXCEPT ![n] = NoWGrants]
  /\ sl' = [sl EXCEPT ![n] = [term |-> 0, exp |-> NONE]]
  /\ UNCHANGED <<now, rec, propOn, prop, acc, wrec, T, fenced, learned>>

Fence(n) ==    \* STONITH by the operator
  /\ ~fenced[n] /\ Cardinality({m \in Nodes : fenced[m]}) < MaxFences
  /\ fenced' = [fenced EXCEPT ![n] = TRUE]
  /\ UNCHANGED <<now, rec, propOn, prop, acc, wrec, T, isPrim, boot, lastGrant,
                 restarted, learned, A, pdeg, hg, hw, sl, nRestarts>>

SGrant(p, m) ==
  /\ Standby /\ Auth(p) /\ m \in A[p] \ {p} /\ T[m] = T[p] /\ ~fenced[m]
  /\ sl' = [sl EXCEPT ![m] = [term |-> T[p],
                              exp  |-> IF FixM2 THEN Min({now + D, End(p)}) ELSE now + D]]
  /\ UNCHANGED <<now, rec, propOn, prop, acc, wrec, T, isPrim, fenced, boot,
                 lastGrant, restarted, learned, A, pdeg, hg, hw, nRestarts>>

Next ==
  \/ Tick
  \/ \E m, n \in Nodes : Grant(m, n) \/ SGrant(m, n) \/ AddA(m, n) \/ Grow(m, n)
  \/ \E w \in Witnesses, n \in Nodes : WGrant(w, n)
  \/ \E w \in Witnesses : WAccept(w)
  \/ \E n \in Nodes : Propose(n) \/ Shrink(n) \/ Ready(n) \/ Learn(n)
                      \/ Restart(n) \/ Fence(n)

Spec == Init /\ [][Next]_vars

(* --- properties ------------------------------------------------------- *)
OneAuthority == \A a, b \in Nodes : a # b => ~(Auth(a) /\ Auth(b))
NoStaleStandby == \A m, n \in Nodes :
                    ~(StandbyValid(m) /\ Auth(n) /\ T[n] > sl[m].term)
Safety == OneAuthority /\ NoStaleStandby
=============================================================================
