---------------------------- MODULE Combined ----------------------------
(***************************************************************************)
(* SNC-AFR-001: authority and durability together (Codex carry-forward 3).*)
(* Verification artifact, not product code.                                *)
(*                                                                         *)
(* Authority.tla checks Claim L in detail, and Log.tla checks Claim D      *)
(* assuming authority. This model joins them where they meet:              *)
(*   - an acknowledgement needs valid authority at that instant;          *)
(*   - promotions, the learn barrier, member leases and degraded witness   *)
(*     leases interleave with shipping, acks and membership changes        *)
(*     (add to A, grow, shrink, drop, degrade);                            *)
(*   - restarts lose volatile state.                                       *)
(*                                                                         *)
(* Abstractions, each covered in detail by another model (see            *)
(* docs/AFR_001_MODEL_COMPOSITION.md for what each assumes of the others): *)
(*   - Time: countdown timers, exact, as in Authority.tla.                 *)
(*   - Witness Paxos: a promotion is chosen atomically, and the witness    *)
(*     group's degraded lease is one timer granted only per the chosen     *)
(*     record (W1). Per-witness acceptance is in Authority.tla.            *)
(*   - Reconciliation: one atomic KIP-279 truncation step against the      *)
(*     current primary. Durable intents, checkpoints and reseeding are     *)
(*     member-local and checked in Log.tla.                                *)
(*   - Operator promotion: inside S without READY (OpInside) and outside  *)
(*     S from a degraded READY record (OpOutsideDeg) are switches. Fencing *)
(*     and standby leases are left out; Authority.tla checks them.         *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS Nodes, P0, Q0, D, MaxTerm, MaxIds, MaxRestarts,
          FixBarrier, FixAckAuth, FixGrow, FixDrop,  \* TRUE = protocol; FALSE = mutation check
          FixGrantTerm,  \* R4: a member grants only to the primary of its own term
          FixLearnStep,  \* R6.0/R4: learning a newer term ends primary status
          FixAinS,       \* R1: a member joins S only from A
          FixW1Term,     \* R8 W1: witness grants only per the chosen record's term
          OpInside,      \* R7: operator promotion inside S, without READY
          OpOutsideDeg,  \* R7 5b: operator promotion outside S from a degraded, READY record
          R2p,           \* R2' candidate (docs/AFR_001_R2_AND_SINGLE_COPY.md 2.2, 4.2)
          InitAll,       \* start with S = A = Nodes
          FixPosGrant,   \* R2' rule 2: a member grants a lease only once positioned
          FixPosAck,     \* R2' rule 2: a member counts toward an ack only once positioned
          FixReadyPos    \* R2' rule 4: READY only once every other member of S is positioned

VARIABLES rec, T, isPrim, learnW, A, pdeg, hg, wl,   \* authority
          dlog, ilog, pos, inh, pack, acked, nid,    \* durability
          nRestarts

vars == <<rec, T, isPrim, learnW, A, pdeg, hg, wl,
          dlog, ilog, pos, inh, pack, acked, nid, nRestarts>>
auth == <<rec, T, isPrim, learnW, A, pdeg, hg, wl>>
logv == <<dlog, ilog, pos, inh, pack, acked, nid>>

Min(S) == CHOOSE x \in S : \A y \in S : x <= y
Max(S) == CHOOSE x \in S : \A y \in S : x >= y
Dec(x) == IF x > 0 THEN x - 1 ELSE 0
Prefix(s, u) == Len(s) <= Len(u) /\ SubSeq(u, 1, Len(s)) = s
LastT(l) == IF Len(l) = 0 THEN 0 ELSE l[Len(l)].t
Terms(l) == {l[i].t : i \in 1..Len(l)} \cup {0}
EndLE(l, t) == Max({i \in 0..Len(l) : i = 0 \/ l[i].t <= t})
NoGrants == [m \in Nodes |-> 0]

S0 == IF InitAll THEN Nodes ELSE {P0, Q0}

Init ==
  /\ rec = [term |-> 1, prim |-> P0, S |-> S0, ready |-> TRUE, deg |-> FALSE]
  /\ T = [n \in Nodes |-> 1]
  /\ isPrim = [n \in Nodes |-> n = P0]
  /\ learnW = [n \in Nodes |-> 0]
  /\ A = [n \in Nodes |-> IF n = P0 THEN S0 ELSE {}]
  /\ pdeg = [n \in Nodes |-> FALSE]
  /\ hg = [n \in Nodes |-> IF n = P0 THEN [m \in Nodes |-> IF m \in S0 \ {P0} THEN D ELSE 0]
                                       ELSE NoGrants]
  /\ wl = [n \in Nodes |-> 0]
  /\ dlog = [n \in Nodes |-> <<>>]
  /\ ilog = [n \in Nodes |-> <<>>]
  /\ pos = [n \in Nodes |-> IF n \in S0 THEN 1 ELSE 0]
  /\ inh = [n \in Nodes |-> 0]
  /\ pack = [n \in Nodes |-> 0]
  /\ acked = {}
  /\ nid = 0
  /\ nRestarts = 0

(* --- authority (R3 simplified: learned + W) ------------------------------ *)
Auth(n) == /\ isPrim[n] /\ (FixBarrier => learnW[n] = 0)
           /\ \A m \in A[n] \ {n} : hg[n][m] > 0
           /\ (pdeg[n] => wl[n] > 0)
Current(p) == isPrim[p] /\ rec.prim = p /\ rec.term = T[p]

Tick ==
  /\ learnW' = [n \in Nodes |-> Dec(learnW[n])]
  /\ hg' = [n \in Nodes |-> [m \in Nodes |-> Dec(hg[n][m])]]
  /\ wl' = [n \in Nodes |-> Dec(wl[n])]
  /\ UNCHANGED <<rec, T, isPrim, A, pdeg, logv, nRestarts>>

Grant(m, n) ==
  /\ m # n /\ isPrim[n] /\ (FixGrantTerm => T[m] = T[n]) /\ m \in A[n]
  /\ (R2p /\ FixPosGrant) => pos[m] = T[n]
  /\ hg' = [hg EXCEPT ![n][m] = D]
  /\ UNCHANGED <<rec, T, isPrim, learnW, A, pdeg, wl, logv, nRestarts>>

WGrant(n) ==   \* W1 against the chosen record
  /\ isPrim[n] /\ pdeg[n] /\ (FixW1Term => rec.term = T[n]) /\ rec.prim = n /\ rec.deg
  /\ wl' = [wl EXCEPT ![n] = D]
  /\ UNCHANGED <<rec, T, isPrim, learnW, A, pdeg, hg, logv, nRestarts>>

NewS(j) == IF R2p /\ j \in rec.S THEN rec.S \ {rec.prim} ELSE {j}

Promote(j) ==  \* R2: j in S and READY; new S = {j}, degraded. R2': new S = S \ {old primary};
               \* the sole member of S re-promotes itself after a restart (4.2)
  /\ rec.term < MaxTerm
  /\ \/ j # rec.prim /\ j \in rec.S /\ (rec.ready \/ OpInside)
     \/ j # rec.prim /\ OpOutsideDeg /\ j \notin rec.S /\ rec.deg /\ rec.ready
     \/ R2p /\ j = rec.prim /\ rec.S = {j} /\ rec.ready /\ ~isPrim[j] /\ T[j] = rec.term
  /\ rec' = [term |-> rec.term + 1, prim |-> j, S |-> NewS(j), ready |-> FALSE,
             deg |-> NewS(j) = {j}]
  /\ UNCHANGED <<T, isPrim, learnW, A, pdeg, hg, wl, logv, nRestarts>>

Learn(n) ==
  /\ rec.term > T[n]
  /\ T' = [T EXCEPT ![n] = rec.term]
  /\ isPrim' = [isPrim EXCEPT ![n] = (rec.prim = n) \/ (~FixLearnStep /\ @)]
  /\ learnW' = [learnW EXCEPT ![n] = IF rec.prim = n THEN D ELSE @]
  /\ A' = [A EXCEPT ![n] = IF rec.prim = n THEN (IF R2p THEN rec.S ELSE {n}) ELSE {}]
  /\ pdeg' = [pdeg EXCEPT ![n] = (rec.prim = n) /\ (R2p => rec.deg)]
  /\ hg' = [hg EXCEPT ![n] = NoGrants]
  /\ wl' = [wl EXCEPT ![n] = 0]
  /\ ilog' = [ilog EXCEPT ![n] = dlog[n]]                  \* R6.1 drain
  /\ pos' = [pos EXCEPT ![n] = IF rec.prim = n THEN rec.term ELSE 0]
  /\ inh' = [inh EXCEPT ![n] = IF rec.prim = n THEN Len(dlog[n]) ELSE 0]
  /\ pack' = [pack EXCEPT ![n] = 0]
  /\ UNCHANGED <<rec, dlog, acked, nid, nRestarts>>

Ready(p) ==
  /\ Current(p) /\ ~rec.ready /\ learnW[p] = 0
  /\ (R2p /\ FixReadyPos) => \A m \in rec.S \ {p} : pos[m] = T[p] /\ T[m] = T[p]
  /\ rec' = [rec EXCEPT !.ready = TRUE]
  /\ UNCHANGED <<T, isPrim, learnW, A, pdeg, hg, wl, logv, nRestarts>>

Restart(n) ==  \* volatile state lost; a primary stops for good in its term (R6.0)
  /\ nRestarts < MaxRestarts
  /\ nRestarts' = nRestarts + 1
  /\ isPrim' = [isPrim EXCEPT ![n] = FALSE]
  /\ A' = [A EXCEPT ![n] = {}]
  /\ pdeg' = [pdeg EXCEPT ![n] = FALSE]
  /\ hg' = [hg EXCEPT ![n] = NoGrants]
  /\ wl' = [wl EXCEPT ![n] = 0]
  /\ ilog' = [ilog EXCEPT ![n] = dlog[n]]
  /\ pos' = [pos EXCEPT ![n] = 0]
  /\ UNCHANGED <<rec, T, learnW, dlog, inh, pack, acked, nid>>

(* --- membership (R1) ----------------------------------------------------- *)
AddA(p, c) ==
  /\ Current(p) /\ c # p /\ c \notin A[p] /\ pos[c] = T[p] /\ T[c] = T[p]
  /\ A' = [A EXCEPT ![p] = @ \cup {c}]
  /\ UNCHANGED <<rec, T, isPrim, learnW, pdeg, hg, wl, logv, nRestarts>>

Grow(p, c) ==  \* caught up: positioned, durable end >= max(inherited end, own acks)
  /\ Current(p) /\ (FixAinS => c \in A[p]) /\ c \notin rec.S /\ pos[c] = T[p]
  /\ Len(dlog[c]) >= IF FixGrow THEN Max({inh[p], pack[p]}) ELSE pack[p]
  /\ rec' = [rec EXCEPT !.S = @ \cup {c}, !.deg = FALSE]
  /\ pdeg' = [pdeg EXCEPT ![p] = FALSE]
  /\ UNCHANGED <<T, isPrim, learnW, A, hg, wl, logv, nRestarts>>

Shrink(p, c) ==  \* remove c from S; S = {p} makes the record degraded
  /\ Current(p) /\ c \in rec.S /\ c # p
  /\ rec' = [rec EXCEPT !.S = @ \ {c}, !.deg = (rec.S \ {c} = {p})]
  /\ pdeg' = [pdeg EXCEPT ![p] = (rec.S \ {c} = {p})]
  /\ UNCHANGED <<T, isPrim, learnW, A, hg, wl, logv, nRestarts>>

Drop(p, c) ==    \* R1.2: only once a read of R, still at p's term, shows c outside S
  /\ isPrim[p] /\ c \in A[p] /\ c # p /\ c \notin rec.S /\ (FixDrop => rec.term = T[p])
  /\ A' = [A EXCEPT ![p] = @ \ {c}]
  /\ UNCHANGED <<rec, T, isPrim, learnW, pdeg, hg, wl, logv, nRestarts>>

(* --- log ----------------------------------------------------------------- *)
NewEntry(p) ==
  /\ Auth(p) /\ nid < MaxIds
  /\ ilog' = [ilog EXCEPT ![p] = Append(@, [t |-> T[p], id |-> nid])]
  /\ nid' = nid + 1
  /\ UNCHANGED <<auth, dlog, pos, inh, pack, acked, nRestarts>>

WriteLocal(p) ==
  /\ isPrim[p] /\ Len(dlog[p]) < Len(ilog[p])
  /\ dlog' = [dlog EXCEPT ![p] = Append(@, ilog[p][Len(dlog[p]) + 1])]
  /\ UNCHANGED <<auth, ilog, pos, inh, pack, acked, nid, nRestarts>>

Ship(p, m) ==    \* a member accepts only from the primary of its own term (R4)
  /\ isPrim[p] /\ m # p /\ pos[m] = T[p] /\ T[m] = T[p] /\ Len(dlog[m]) < Len(ilog[p])
  /\ dlog' = [dlog EXCEPT ![m] = Append(@, ilog[p][Len(dlog[m]) + 1])]
  /\ UNCHANGED <<auth, ilog, pos, inh, pack, acked, nid, nRestarts>>

Ack(p, i) ==     \* needs valid authority now, and every member of A holding it
  /\ isPrim[p] /\ (FixAckAuth => Auth(p)) /\ i \in 1..Len(dlog[p])
  /\ \A m \in A[p] : /\ Len(dlog[m]) >= i /\ dlog[m][i] = dlog[p][i]
                     /\ (m = p \/ (T[m] = T[p] /\ ((R2p /\ FixPosAck) => pos[m] = T[p])))
  /\ acked' = acked \cup {<<i, dlog[p][i]>>}
  /\ pack' = [pack EXCEPT ![p] = Max({@, i})]
  /\ UNCHANGED <<auth, dlog, ilog, pos, inh, nid, nRestarts>>

Recon(k, j) ==   \* one KIP-279 step against the current primary (R6.2)
  /\ isPrim[j] /\ k # j /\ T[k] = T[j] /\ pos[k] # T[j]
  /\ LET ts == Max({x \in Terms(ilog[j]) : x <= LastT(dlog[k])})
         L  == Min({EndLE(ilog[j], ts), EndLE(dlog[k], ts)})
     IN IF L = Len(dlog[k]) /\ ts = LastT(dlog[k])
        THEN pos' = [pos EXCEPT ![k] = T[j]] /\ UNCHANGED dlog
        ELSE dlog' = [dlog EXCEPT ![k] = SubSeq(@, 1, L)] /\ UNCHANGED pos
  /\ UNCHANGED <<auth, ilog, inh, pack, acked, nid, nRestarts>>

Next ==
  \/ Tick
  \/ \E n \in Nodes : Promote(n) \/ Learn(n) \/ Ready(n) \/ Restart(n) \/ WGrant(n)
                      \/ NewEntry(n) \/ WriteLocal(n)
  \/ \E m, n \in Nodes : Grant(m, n) \/ AddA(m, n) \/ Grow(m, n) \/ Shrink(m, n)
                         \/ Drop(m, n) \/ Ship(m, n) \/ Recon(m, n)
  \/ \E p \in Nodes, i \in 1..MaxIds : Ack(p, i)

Spec == Init /\ [][Next]_vars

(* --- properties ---------------------------------------------------------- *)
OneAuthority == \A a, b \in Nodes : a # b => ~(Auth(a) /\ Auth(b))
AckedPreserved == \A e \in acked : \A n \in rec.S :
                    Len(dlog[n]) >= e[1] /\ dlog[n][e[1]] = e[2]
EntryUnique == \A a, b \in Nodes : \A i \in 1..Min({Len(dlog[a]), Len(dlog[b])}) :
                 dlog[a][i].t = dlog[b][i].t => dlog[a][i] = dlog[b][i]
LogMatching == \A a, b \in Nodes : \A i \in 1..Min({Len(dlog[a]), Len(dlog[b])}) :
                 dlog[a][i] = dlog[b][i] => SubSeq(dlog[a], 1, i) = SubSeq(dlog[b], 1, i)
\* every acknowledged entry is in the log of the current primary once it has learned
AckedAtPrimary == \A e \in acked : \A n \in Nodes :
                    (isPrim[n] /\ T[n] = rec.term) =>
                       (Len(dlog[n]) >= e[1] /\ dlog[n][e[1]] = e[2])
Safety == OneAuthority /\ AckedPreserved /\ EntryUnique /\ LogMatching /\ AckedAtPrimary

(* --- Auth and the group record (Codex concern 3 on PR #144) -------------- *)
\* Literal form: valid authority only at the record's term. Expected to FAIL by
\* design: an old primary keeps its lease until it expires, after the new term
\* is chosen; the new primary waits learned + W instead.
AuthAtRecTerm == \A n \in Nodes : Auth(n) => T[n] = rec.term
\* What holds instead (interface invariant, same as StaleAuthBounded in
\* Authority.tla): authority below the record's term ends no later than the new
\* primary's learn barrier, and the new primary has none meanwhile.
Rem(n) == Min({hg[n][m] : m \in A[n] \ {n}} \cup (IF pdeg[n] THEN {wl[n]} ELSE {}) \cup {D})
StaleAuthBounded == \A n \in Nodes : (Auth(n) /\ T[n] # rec.term) =>
   /\ ~(Auth(rec.prim) /\ T[rec.prim] = rec.term)
   /\ ((isPrim[rec.prim] /\ T[rec.prim] = rec.term) => Rem(n) <= learnW[rec.prim])
\* Stronger form, true only without operator promotion: stale authority is one
\* term behind and still holds a lease from the promoted member.
OneTermBehind == \A n \in Nodes : (Auth(n) /\ T[n] # rec.term) =>
   T[n] = rec.term - 1 /\ rec.prim \in A[n]
=============================================================================
