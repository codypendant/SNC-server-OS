---------------------------- MODULE Authority ----------------------------
(***************************************************************************)
(* SNC-AFR-001 Claim L: at most one node holds valid authority.            *)
(* Verification artifact for docs/AFR_001_SAFETY_ANALYSIS.md, R1-R9.       *)
(* Not product code.                                                       *)
(*                                                                         *)
(* Time is kept as countdown timers, not as absolute timestamps.           *)
(*   - Every timer holds the real time left, from 0 to D.                  *)
(*   - Tick lowers each nonzero timer by 1.                                *)
(*   - A grant sets its timer to D, and the lease holds while the timer    *)
(*     is above 0.                                                         *)
(*   - A wait sets its timer to D, and the wait is over at 0.              *)
(* Every timing predicate compares a stored time with "now" plus D. This  *)
(* is therefore an exact quotient of the absolute-time model by time       *)
(* shift, not an approximation. It also removes any bound on how long the *)
(* system runs. Drift inside the stated bound is folded into D (section 0 *)
(* of the analysis). Drift beyond it is out of contract and not modelled. *)
(*                                                                         *)
(* The group record is a single register; witness Paxos is abstracted.    *)
(*   - A promotion is accepted witness by witness, and is chosen at a      *)
(*     majority. Its proposer is modelled as never failing.                *)
(*   - Other compare-and-sets (grow, shrink, ready) are atomic, and are    *)
(*     disabled while a promotion is pending.                              *)
(*                                                                         *)
(* Each Fix* constant switches one fix on. Turning a fix off must let TLC  *)
(* find the failure that fix prevents.                                     *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS Nodes, Witnesses, P0, Q0, D, MaxTerm, MaxRestarts, MaxFences,
          R3Full,     \* R3 as in Revision 2's draft: boot, last-grant and learned bounds
                      \* (FALSE: the simplified rule, learned + W only)
          FixR3,      \* track grants made after a restart (Revision 1 did not)
          Barrier,    \* R3/R8: wait D after learning the promotion was chosen
          FixReady,   \* R2: READY only after the primary is eligible (B2)
          W1,         \* R8: a witness grants only per its accepted record
          FixM2,      \* R9: a standby lease never outlasts the primary's
          FixOp,      \* R7: outside-S from degraded needs READY or fencing
          OpInside,   \* allow operator promotion inside S without READY
          AllowOp5c,  \* allow outside-S promotion with |S|>=2 unfenced (5c)
          FixBootRead,\* R7: a fenced node that comes back reads R before it grants
          OpOutside,  \* allow operator promotion outside S at all
          Degrade,    \* allow the degrade step (Shrink)
          Standby     \* allow standby read leases

VARIABLES rec, propOn, prop, acc, wrec,
          T, isPrim, fenced, restarted,
          bootW, grantW, learnW,   \* R3 wait timers
          A, pdeg, hg, hw, sl, nRestarts

vars == <<rec, propOn, prop, acc, wrec, T, isPrim, fenced, restarted,
          bootW, grantW, learnW, A, pdeg, hg, hw, sl, nRestarts>>

Maj(Q) == 2 * Cardinality(Q) > Cardinality(Witnesses)
Min(S) == CHOOSE x \in S : \A y \in S : x <= y
Max(S) == CHOOSE x \in S : \A y \in S : x >= y
Dec(x) == IF x > 0 THEN x - 1 ELSE 0
NoGrants == [m \in Nodes |-> 0]
NoWGrants == [w \in Witnesses |-> 0]

Init ==
  /\ rec = [term |-> 1, prim |-> P0, S |-> {P0, Q0}, ready |-> TRUE, deg |-> FALSE]
  /\ propOn = FALSE
  /\ prop = rec
  /\ acc = {}
  /\ wrec = [w \in Witnesses |-> rec]
  /\ T = [n \in Nodes |-> 1]
  /\ isPrim = [n \in Nodes |-> n = P0]
  /\ fenced = [n \in Nodes |-> FALSE]
  /\ restarted = [n \in Nodes |-> FALSE]
  /\ bootW = [n \in Nodes |-> 0]
  /\ grantW = [n \in Nodes |-> IF n = Q0 THEN D ELSE 0]
  /\ learnW = [n \in Nodes |-> 0]
  /\ A = [n \in Nodes |-> IF n = P0 THEN {P0, Q0} ELSE {}]
  /\ pdeg = [n \in Nodes |-> FALSE]
  /\ hg = [n \in Nodes |-> IF n = P0 THEN [NoGrants EXCEPT ![Q0] = D] ELSE NoGrants]
  /\ hw = [n \in Nodes |-> NoWGrants]
  /\ sl = [n \in Nodes |-> [term |-> 0, rem |-> 0]]
  /\ nRestarts = 0

(* --- authority -------------------------------------------------------- *)
Eligible(n) ==
  /\ isPrim[n] /\ ~fenced[n]
  /\ (Barrier => learnW[n] = 0)
  /\ (R3Full => (bootW[n] = 0 /\ grantW[n] = 0))

MemberLease(n) == \A m \in A[n] \ {n} : hg[n][m] > 0
WitnessLease(n) == \E Q \in SUBSET Witnesses : Maj(Q) /\ \A w \in Q : hw[n][w] > 0
Auth(n) == Eligible(n) /\ MemberLease(n) /\ (pdeg[n] => WitnessLease(n))

\* real time left on p's authority
MRem(n) == IF A[n] \ {n} = {} THEN D ELSE Min({hg[n][m] : m \in A[n] \ {n}})
WRem(n) == Max({e \in {hw[n][w] : w \in Witnesses} :
                 Maj({w \in Witnesses : hw[n][w] >= e})})
Rem(n) == IF pdeg[n] THEN Min({MRem(n), WRem(n)}) ELSE MRem(n)

StandbyValid(m) == ~fenced[m] /\ sl[m].term = T[m] /\ sl[m].rem > 0

Current(p) == isPrim[p] /\ ~fenced[p] /\ rec.prim = p /\ rec.term = T[p]

(* --- actions ---------------------------------------------------------- *)
Tick ==
  /\ bootW' = [n \in Nodes |-> Dec(bootW[n])]
  /\ grantW' = [n \in Nodes |-> Dec(grantW[n])]
  /\ learnW' = [n \in Nodes |-> Dec(learnW[n])]
  /\ hg' = [n \in Nodes |-> [m \in Nodes |-> Dec(hg[n][m])]]
  /\ hw' = [n \in Nodes |-> [w \in Witnesses |-> Dec(hw[n][w])]]
  /\ sl' = [n \in Nodes |-> [sl[n] EXCEPT !.rem = Dec(@)]]
  /\ UNCHANGED <<rec, propOn, prop, acc, wrec, T, isPrim, fenced, restarted,
                 A, pdeg, nRestarts>>

\* member m grants to the primary n of m's own known term
Grant(m, n) ==
  /\ m # n /\ ~fenced[m] /\ isPrim[n] /\ ~fenced[n] /\ T[m] = T[n] /\ m \in A[n]
  /\ hg' = [hg EXCEPT ![n][m] = D]
  /\ grantW' = [grantW EXCEPT ![m] = IF FixR3 \/ ~restarted[m] THEN D ELSE @]
  /\ UNCHANGED <<rec, propOn, prop, acc, wrec, T, isPrim, fenced, restarted,
                 bootW, learnW, A, pdeg, hw, sl, nRestarts>>

WGrant(w, n) ==
  /\ isPrim[n] /\ ~fenced[n] /\ pdeg[n]
  /\ (W1 => (wrec[w].term = T[n] /\ wrec[w].prim = n /\ wrec[w].deg))
  /\ hw' = [hw EXCEPT ![n][w] = D]
  /\ UNCHANGED <<rec, propOn, prop, acc, wrec, T, isPrim, fenced, restarted,
                 bootW, grantW, learnW, A, pdeg, hg, sl, nRestarts>>

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
  /\ UNCHANGED <<rec, wrec, T, isPrim, fenced, restarted, bootW, grantW, learnW,
                 A, pdeg, hg, hw, sl, nRestarts>>

WAccept(w) ==
  /\ propOn /\ w \notin acc
  /\ wrec' = [wrec EXCEPT ![w] = prop]
  /\ acc' = acc \cup {w}
  /\ IF Maj(acc \cup {w})
       THEN rec' = prop /\ propOn' = FALSE
       ELSE UNCHANGED <<rec, propOn>>
  /\ UNCHANGED <<prop, T, isPrim, fenced, restarted, bootW, grantW, learnW,
                 A, pdeg, hg, hw, sl, nRestarts>>

\* atomic non-term compare-and-set by the current primary
SetRec(r) == rec' = r /\ wrec' = [w \in Witnesses |-> r]

AddA(p, c) ==
  /\ Current(p) /\ ~propOn /\ c # p /\ c \notin A[p] /\ ~fenced[c]
  /\ A' = [A EXCEPT ![p] = @ \cup {c}]
  /\ UNCHANGED <<rec, propOn, prop, acc, wrec, T, isPrim, fenced, restarted,
                 bootW, grantW, learnW, pdeg, hg, hw, sl, nRestarts>>

Grow(p, c) ==
  /\ Current(p) /\ ~propOn /\ c \in A[p] /\ c \notin rec.S
  /\ SetRec([rec EXCEPT !.S = @ \cup {c}, !.deg = FALSE])
  /\ pdeg' = [pdeg EXCEPT ![p] = FALSE]
  /\ UNCHANGED <<propOn, prop, acc, T, isPrim, fenced, restarted, bootW, grantW,
                 learnW, A, hg, hw, sl, nRestarts>>

Shrink(p) ==   \* degrade: S becomes {p}; R1.2 lets p drop the others from A
  /\ Degrade /\ Current(p) /\ ~propOn /\ rec.S # {p}
  /\ SetRec([rec EXCEPT !.S = {p}, !.deg = TRUE])
  /\ pdeg' = [pdeg EXCEPT ![p] = TRUE]
  /\ A' = [A EXCEPT ![p] = {p}]
  /\ UNCHANGED <<propOn, prop, acc, T, isPrim, fenced, restarted, bootW, grantW,
                 learnW, hg, hw, sl, nRestarts>>

Ready(p) ==
  /\ Current(p) /\ ~propOn /\ ~rec.ready
  /\ (FixReady => Eligible(p))
  /\ SetRec([rec EXCEPT !.ready = TRUE])
  /\ UNCHANGED <<propOn, prop, acc, T, isPrim, fenced, restarted, bootW, grantW,
                 learnW, A, pdeg, hg, hw, sl, nRestarts>>

Learn(n) ==
  /\ ~fenced[n] /\ rec.term > T[n]
  /\ T' = [T EXCEPT ![n] = rec.term]
  /\ isPrim' = [isPrim EXCEPT ![n] = (rec.prim = n)]
  /\ learnW' = [learnW EXCEPT ![n] = IF rec.prim = n THEN D ELSE @]
  /\ A' = [A EXCEPT ![n] = IF rec.prim = n THEN {n} ELSE {}]
  /\ pdeg' = [pdeg EXCEPT ![n] = (rec.prim = n)]
  /\ hg' = [hg EXCEPT ![n] = NoGrants]
  /\ hw' = [hw EXCEPT ![n] = NoWGrants]
  /\ UNCHANGED <<rec, propOn, prop, acc, wrec, fenced, restarted, bootW, grantW,
                 sl, nRestarts>>

Restart(n) ==
  /\ ~fenced[n] /\ nRestarts < MaxRestarts
  /\ nRestarts' = nRestarts + 1
  /\ bootW' = [bootW EXCEPT ![n] = D]
  /\ grantW' = [grantW EXCEPT ![n] = 0]               \* grant memory is lost
  /\ restarted' = [restarted EXCEPT ![n] = TRUE]
  /\ isPrim' = [isPrim EXCEPT ![n] = FALSE]          \* R4 / R6.0
  /\ A' = [A EXCEPT ![n] = {}]
  /\ pdeg' = [pdeg EXCEPT ![n] = FALSE]
  /\ hg' = [hg EXCEPT ![n] = NoGrants]
  /\ hw' = [hw EXCEPT ![n] = NoWGrants]
  /\ sl' = [sl EXCEPT ![n] = [term |-> 0, rem |-> 0]]
  /\ UNCHANGED <<rec, propOn, prop, acc, wrec, T, fenced, learnW>>

Fence(n) ==    \* STONITH by the operator: n stops
  /\ ~fenced[n] /\ Cardinality({m \in Nodes : fenced[m]}) < MaxFences
  /\ fenced' = [fenced EXCEPT ![n] = TRUE]
  /\ UNCHANGED <<rec, propOn, prop, acc, wrec, T, isPrim, restarted, bootW, grantW,
                 learnW, A, pdeg, hg, hw, sl, nRestarts>>

Unfence(n) ==  \* a fenced node comes back (reboot or power-on), as after Restart
  /\ fenced[n] /\ nRestarts < MaxRestarts
  /\ nRestarts' = nRestarts + 1
  /\ fenced' = [fenced EXCEPT ![n] = FALSE]
  \* R7 boot read: it reads R from the witnesses before it grants
  /\ T' = [T EXCEPT ![n] = IF FixBootRead /\ rec.term > @ THEN rec.term ELSE @]
  /\ bootW' = [bootW EXCEPT ![n] = D]
  /\ grantW' = [grantW EXCEPT ![n] = 0]
  /\ restarted' = [restarted EXCEPT ![n] = TRUE]
  /\ isPrim' = [isPrim EXCEPT ![n] = FALSE]
  /\ A' = [A EXCEPT ![n] = {}]
  /\ pdeg' = [pdeg EXCEPT ![n] = FALSE]
  /\ hg' = [hg EXCEPT ![n] = NoGrants]
  /\ hw' = [hw EXCEPT ![n] = NoWGrants]
  /\ sl' = [sl EXCEPT ![n] = [term |-> 0, rem |-> 0]]
  /\ UNCHANGED <<rec, propOn, prop, acc, wrec, learnW>>

SGrant(p, m) ==
  /\ Standby /\ Auth(p) /\ m \in A[p] \ {p} /\ T[m] = T[p] /\ ~fenced[m]
  /\ sl' = [sl EXCEPT ![m] = [term |-> T[p],
                              rem  |-> IF FixM2 THEN Min({D, Rem(p)}) ELSE D]]
  /\ UNCHANGED <<rec, propOn, prop, acc, wrec, T, isPrim, fenced, restarted,
                 bootW, grantW, learnW, A, pdeg, hg, hw, nRestarts>>

Next ==
  \/ Tick
  \/ \E m, n \in Nodes : Grant(m, n) \/ SGrant(m, n) \/ AddA(m, n) \/ Grow(m, n)
  \/ \E w \in Witnesses, n \in Nodes : WGrant(w, n)
  \/ \E w \in Witnesses : WAccept(w)
  \/ \E n \in Nodes : Propose(n) \/ Shrink(n) \/ Ready(n) \/ Learn(n)
                      \/ Restart(n) \/ Fence(n) \/ Unfence(n)

Spec == Init /\ [][Next]_vars

(* --- properties ------------------------------------------------------- *)
OneAuthority == \A a, b \in Nodes : a # b => ~(Auth(a) /\ Auth(b))
NoStaleStandby == \A m, n \in Nodes :
                    ~(StandbyValid(m) /\ Auth(n) /\ T[n] > sl[m].term)
\* R3 lemma: for a primary, the learned wait outlasts the boot and grant waits,
\* so the simplified rule (learned + W) implies the full one.
R3Lemma == \A n \in Nodes : isPrim[n] => (learnW[n] >= bootW[n] /\ learnW[n] >= grantW[n])
Safety == OneAuthority /\ NoStaleStandby
=============================================================================
