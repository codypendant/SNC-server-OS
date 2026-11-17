--------------------------- MODULE StandbyLease ---------------------------
(***************************************************************************)
(* SNC-AFR-001: rule R9 (standby read leases) against writes, G-30.       *)
(* Verification artifact, not product code.                                *)
(*                                                                         *)
(* G-30: a standby read under a valid lease reflects every write          *)
(* acknowledged before the read. R9: while a lease p granted to m may     *)
(* still be valid, p does not degrade, does not remove m from A, and does *)
(* not acknowledge without m.                                              *)
(*                                                                         *)
(* This is Combined.tla (authority + durability, same abstractions, same  *)
(* time model) with what that model left out:                              *)
(*   - standby read leases: SGrant sets the standby's lease sl[m] and the *)
(*     primary's own record sw[p][m] of how long that lease may last       *)
(*     (R9's wait W, which drift folds into D as in the other models);     *)
(*   - an applied prefix app[m] next to the durable log, so the order of   *)
(*     apply and acknowledge is visible;                                   *)
(*   - fencing (STONITH) and a fenced node's return with the R7 boot read, *)
(*     and operator promotion outside S once every member of S is fenced. *)
(*                                                                         *)
(* A grant is atomic: the standby's lease and p's record start together.  *)
(* A grant delayed in flight is anchored at its send time, so it ends no  *)
(* later than this one; the atomic grant is the worst case.               *)
(*                                                                         *)
(* A standby keeps serving under its lease after it learns a newer term   *)
(* unless StandbyStopOnLearn is TRUE (R4 says it stops). The committed    *)
(* configuration leaves it FALSE, so the result does not lean on R4 here. *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS Nodes, P0, Q0, D, MaxTerm, MaxIds, MaxRestarts, MaxFences,
          \* R9 guards. TRUE = protocol; FALSE = mutation check
          FixShrinkWait,  \* no degrade/shrink of m while m's lease may be valid
          FixDropWait,    \* no removal of m from A while m's lease may be valid
          FixAckA,        \* an ack needs every member of A, not only those still in S
          AckRule,        \* what a member's ack reports: "applied" (protocol: every member
                          \* acks only applied entries), "leased" (R9's text: only while it
                          \* holds a lease), "durable" (apply order ignored)
          FixRhoWait,     \* p's wait sw covers the whole lease (FALSE: it ends one unit early,
                          \* as a record without the (1+rho) factor can under drift)
          FixM2,          \* the lease never outlasts p's own authority
          FixLeaseSync,   \* a lease only to a member caught up to everything p must hold
          FixRestartStop, \* R6.0: a restarted primary does not resume its term
          \* guards from Combined.tla that bear on G-30
          FixBarrier, FixAckAuth,
          StandbyStopOnLearn, \* R4 on the standby side (FALSE = do not rely on it)
          OpOutsideDeg,   \* R7: outside-S promotion from a degraded, READY record
          OpFenced        \* R7: outside-S promotion once every member of S is fenced

VARIABLES rec, T, isPrim, learnW, A, pdeg, hg, wl,   \* authority
          dlog, ilog, pos, inh, pack, acked, nid,    \* durability
          app, sl, slt, sw, fenced, nRestarts, mack  \* this model

vars == <<rec, T, isPrim, learnW, A, pdeg, hg, wl,
          dlog, ilog, pos, inh, pack, acked, nid, app, sl, slt, sw, fenced, nRestarts, mack>>
auth == <<rec, T, isPrim, learnW, A, pdeg, hg, wl>>
logv == <<dlog, ilog, pos, inh, pack, acked, nid>>
r9v  == <<app, sl, slt, sw, fenced, nRestarts, mack>>

Min(S) == CHOOSE x \in S : \A y \in S : x <= y
Max(S) == CHOOSE x \in S : \A y \in S : x >= y
Dec(x) == IF x > 0 THEN x - 1 ELSE 0
LastT(l) == IF Len(l) = 0 THEN 0 ELSE l[Len(l)].t
Terms(l) == {l[i].t : i \in 1..Len(l)} \cup {0}
EndLE(l, t) == Max({i \in 0..Len(l) : i = 0 \/ l[i].t <= t})
NoGrants == [m \in Nodes |-> 0]

Init ==
  /\ rec = [term |-> 1, prim |-> P0, S |-> {P0, Q0}, ready |-> TRUE, deg |-> FALSE]
  /\ T = [n \in Nodes |-> 1]
  /\ isPrim = [n \in Nodes |-> n = P0]
  /\ learnW = [n \in Nodes |-> 0]
  /\ A = [n \in Nodes |-> IF n = P0 THEN {P0, Q0} ELSE {}]
  /\ pdeg = [n \in Nodes |-> FALSE]
  /\ hg = [n \in Nodes |-> IF n = P0 THEN [NoGrants EXCEPT ![Q0] = D] ELSE NoGrants]
  /\ wl = [n \in Nodes |-> 0]
  /\ dlog = [n \in Nodes |-> <<>>]
  /\ ilog = [n \in Nodes |-> <<>>]
  /\ pos = [n \in Nodes |-> IF n \in {P0, Q0} THEN 1 ELSE 0]
  /\ inh = [n \in Nodes |-> 0]
  /\ pack = [n \in Nodes |-> 0]
  /\ acked = {}
  /\ nid = 0
  /\ app = [n \in Nodes |-> 0]
  /\ sl = [n \in Nodes |-> 0]
  /\ slt = [n \in Nodes |-> 0]
  /\ sw = [n \in Nodes |-> NoGrants]
  /\ fenced = [n \in Nodes |-> FALSE]
  /\ mack = [n \in Nodes |-> 0]
  /\ nRestarts = 0

Up(n) == ~fenced[n]

(* --- authority (as Combined.tla, plus fencing) --------------------------- *)
Auth(n) == /\ isPrim[n] /\ Up(n) /\ (FixBarrier => learnW[n] = 0)
           /\ \A m \in A[n] \ {n} : hg[n][m] > 0
           /\ (pdeg[n] => wl[n] > 0)
Current(p) == isPrim[p] /\ Up(p) /\ rec.prim = p /\ rec.term = T[p]
\* real time left on p's authority
Rem(n) == Min({hg[n][m] : m \in A[n] \ {n}} \cup (IF pdeg[n] THEN {wl[n]} ELSE {}) \cup {D})

StandbyValid(m) == Up(m) /\ sl[m] > 0

Tick ==
  /\ learnW' = [n \in Nodes |-> Dec(learnW[n])]
  /\ hg' = [n \in Nodes |-> [m \in Nodes |-> Dec(hg[n][m])]]
  /\ wl' = [n \in Nodes |-> Dec(wl[n])]
  /\ sl' = [n \in Nodes |-> Dec(sl[n])]
  /\ sw' = [n \in Nodes |-> [m \in Nodes |-> Dec(sw[n][m])]]
  /\ slt' = [n \in Nodes |-> IF sl[n] > 1 THEN slt[n] ELSE 0]  \* forget a lapsed lease's term
  /\ UNCHANGED <<rec, T, isPrim, A, pdeg, logv, app, fenced, nRestarts, mack>>

Grant(m, n) ==   \* R4: a member grants only to the primary of its own term
  /\ m # n /\ Up(m) /\ isPrim[n] /\ Up(n) /\ T[m] = T[n] /\ m \in A[n]
  /\ hg' = [hg EXCEPT ![n][m] = D]
  /\ UNCHANGED <<rec, T, isPrim, learnW, A, pdeg, wl, logv, r9v>>

WGrant(n) ==     \* W1 against the chosen record
  /\ isPrim[n] /\ Up(n) /\ pdeg[n] /\ rec.term = T[n] /\ rec.prim = n /\ rec.deg
  /\ wl' = [wl EXCEPT ![n] = D]
  /\ UNCHANGED <<rec, T, isPrim, learnW, A, pdeg, hg, logv, r9v>>

\* A lease to m: m in A at p's term, and (FixLeaseSync) positioned on p's log and
\* holding, applied, everything p must hold: what it inherited and what it acked.
SGrant(p, m) ==
  /\ Auth(p) /\ m \in A[p] \ {p} /\ T[m] = T[p] /\ Up(m)
  /\ FixLeaseSync => (pos[m] = T[p] /\ app[m] >= Max({inh[p], pack[p]}))
  /\ sl' = [sl EXCEPT ![m] = IF FixM2 THEN Min({D, Rem(p)}) ELSE D]
  /\ slt' = [slt EXCEPT ![m] = T[p]]
  /\ sw' = [sw EXCEPT ![p][m] = IF FixRhoWait THEN D ELSE D - 1]
  /\ UNCHANGED <<auth, logv, app, fenced, nRestarts, mack>>

AllSFenced == \A s \in rec.S : fenced[s]
Promote(j) ==    \* R2 and R7; new S = {j}, degraded
  /\ rec.term < MaxTerm /\ j # rec.prim /\ Up(j)
  /\ \/ j \in rec.S /\ rec.ready
     \/ OpOutsideDeg /\ j \notin rec.S /\ rec.deg /\ rec.ready
     \/ OpFenced /\ j \notin rec.S /\ AllSFenced
  /\ rec' = [term |-> rec.term + 1, prim |-> j, S |-> {j}, ready |-> FALSE, deg |-> TRUE]
  /\ UNCHANGED <<T, isPrim, learnW, A, pdeg, hg, wl, logv, r9v>>

Learn(n) ==
  /\ Up(n) /\ rec.term > T[n]
  /\ T' = [T EXCEPT ![n] = rec.term]
  /\ isPrim' = [isPrim EXCEPT ![n] = (rec.prim = n)]
  /\ learnW' = [learnW EXCEPT ![n] = IF rec.prim = n THEN D ELSE @]
  /\ A' = [A EXCEPT ![n] = IF rec.prim = n THEN {n} ELSE {}]
  /\ pdeg' = [pdeg EXCEPT ![n] = (rec.prim = n)]
  /\ hg' = [hg EXCEPT ![n] = NoGrants]
  /\ wl' = [wl EXCEPT ![n] = 0]
  /\ ilog' = [ilog EXCEPT ![n] = dlog[n]]                  \* R6.1 drain
  /\ pos' = [pos EXCEPT ![n] = IF rec.prim = n THEN rec.term ELSE 0]
  /\ inh' = [inh EXCEPT ![n] = IF rec.prim = n THEN Len(dlog[n]) ELSE 0]
  /\ pack' = [pack EXCEPT ![n] = 0]
  /\ sl' = [sl EXCEPT ![n] = IF StandbyStopOnLearn THEN 0 ELSE @]
  /\ sw' = [sw EXCEPT ![n] = NoGrants]   \* n is no longer the old term's primary
  /\ slt' = [slt EXCEPT ![n] = IF StandbyStopOnLearn THEN 0 ELSE @]
  /\ mack' = [mack EXCEPT ![n] = 0]   \* R4: acks to an older term do not carry over
  /\ UNCHANGED <<rec, dlog, acked, nid, app, fenced, nRestarts>>

Ready(p) ==
  /\ Current(p) /\ ~rec.ready /\ learnW[p] = 0
  /\ rec' = [rec EXCEPT !.ready = TRUE]
  /\ UNCHANGED <<T, isPrim, learnW, A, pdeg, hg, wl, logv, r9v>>

\* Volatile state is lost; recovery replays the durable log (app = Len(dlog)).
\* Protocol (R6.0): a primary stops for good in its term. Mutation: it resumes
\* its term from durable state with its record of standby leases lost (the
\* C++ keeps lease_until_ in memory only, G-30 evidence).
Restart(n) ==
  /\ Up(n) /\ nRestarts < MaxRestarts
  /\ nRestarts' = nRestarts + 1
  /\ isPrim' = [isPrim EXCEPT ![n] = ~FixRestartStop /\ @]
  /\ A' = [A EXCEPT ![n] = IF ~FixRestartStop /\ isPrim[n] THEN @ ELSE {}]
  /\ pdeg' = [pdeg EXCEPT ![n] = ~FixRestartStop /\ @]
  /\ hg' = [hg EXCEPT ![n] = NoGrants]
  /\ wl' = [wl EXCEPT ![n] = 0]
  /\ ilog' = [ilog EXCEPT ![n] = dlog[n]]
  /\ pos' = [pos EXCEPT ![n] = IF ~FixRestartStop /\ isPrim[n] THEN @ ELSE 0]
  /\ app' = [app EXCEPT ![n] = Len(dlog[n])]
  /\ sl' = [sl EXCEPT ![n] = 0]
  /\ sw' = [sw EXCEPT ![n] = NoGrants]
  /\ slt' = [slt EXCEPT ![n] = 0]
  /\ mack' = [mack EXCEPT ![n] = 0]
  /\ UNCHANGED <<rec, T, learnW, dlog, inh, pack, acked, nid, fenced>>

Fence(n) ==      \* STONITH: n stops; nothing it does counts until it returns
  /\ Up(n) /\ Cardinality({m \in Nodes : fenced[m]}) < MaxFences
  /\ fenced' = [fenced EXCEPT ![n] = TRUE]
  /\ UNCHANGED <<auth, logv, app, sl, slt, sw, nRestarts, mack>>

Unfence(n) ==    \* it comes back as after a restart, and reads R first (R7 boot read)
  /\ fenced[n] /\ nRestarts < MaxRestarts
  /\ nRestarts' = nRestarts + 1
  /\ fenced' = [fenced EXCEPT ![n] = FALSE]
  /\ T' = [T EXCEPT ![n] = Max({@, rec.term})]
  /\ isPrim' = [isPrim EXCEPT ![n] = FALSE]
  /\ A' = [A EXCEPT ![n] = {}]
  /\ pdeg' = [pdeg EXCEPT ![n] = FALSE]
  /\ hg' = [hg EXCEPT ![n] = NoGrants]
  /\ wl' = [wl EXCEPT ![n] = 0]
  /\ ilog' = [ilog EXCEPT ![n] = dlog[n]]
  /\ pos' = [pos EXCEPT ![n] = 0]
  /\ app' = [app EXCEPT ![n] = Len(dlog[n])]
  /\ sl' = [sl EXCEPT ![n] = 0]
  /\ sw' = [sw EXCEPT ![n] = NoGrants]
  /\ slt' = [slt EXCEPT ![n] = 0]
  /\ mack' = [mack EXCEPT ![n] = 0]
  /\ UNCHANGED <<rec, learnW, dlog, inh, pack, acked, nid>>

(* --- membership (R1, R9) ------------------------------------------------- *)
AddA(p, c) ==
  /\ Current(p) /\ c # p /\ Up(c) /\ c \notin A[p] /\ pos[c] = T[p] /\ T[c] = T[p]
  /\ A' = [A EXCEPT ![p] = @ \cup {c}]
  /\ UNCHANGED <<rec, T, isPrim, learnW, pdeg, hg, wl, logv, r9v>>

Grow(p, c) ==    \* caught up: positioned, durable end >= max(inherited end, own acks)
  /\ Current(p) /\ c \in A[p] /\ c \notin rec.S /\ pos[c] = T[p]
  /\ Len(dlog[c]) >= Max({inh[p], pack[p]})
  /\ rec' = [rec EXCEPT !.S = @ \cup {c}, !.deg = FALSE]
  /\ pdeg' = [pdeg EXCEPT ![p] = FALSE]
  /\ UNCHANGED <<T, isPrim, learnW, A, hg, wl, logv, r9v>>

Shrink(p, c) ==  \* remove c from S; S = {p} makes the record degraded
  /\ Current(p) /\ c \in rec.S /\ c # p
  /\ FixShrinkWait => sw[p][c] = 0
  /\ rec' = [rec EXCEPT !.S = @ \ {c}, !.deg = (rec.S \ {c} = {p})]
  /\ pdeg' = [pdeg EXCEPT ![p] = (rec.S \ {c} = {p})]
  /\ UNCHANGED <<T, isPrim, learnW, A, hg, wl, logv, r9v>>

Drop(p, c) ==    \* R1.2 (read of R still at p's term shows c outside S) and R9
  /\ isPrim[p] /\ Up(p) /\ c \in A[p] /\ c # p /\ c \notin rec.S /\ rec.term = T[p]
  /\ FixDropWait => sw[p][c] = 0
  /\ A' = [A EXCEPT ![p] = @ \ {c}]
  /\ UNCHANGED <<rec, T, isPrim, learnW, pdeg, hg, wl, logv, r9v>>

(* --- log ----------------------------------------------------------------- *)
NewEntry(p) ==
  /\ Auth(p) /\ nid < MaxIds
  /\ ilog' = [ilog EXCEPT ![p] = Append(@, [t |-> T[p], id |-> nid])]
  /\ nid' = nid + 1
  /\ UNCHANGED <<auth, dlog, pos, inh, pack, acked, r9v>>

WriteLocal(p) ==
  /\ isPrim[p] /\ Up(p) /\ Len(dlog[p]) < Len(ilog[p])
  /\ dlog' = [dlog EXCEPT ![p] = Append(@, ilog[p][Len(dlog[p]) + 1])]
  /\ UNCHANGED <<auth, ilog, pos, inh, pack, acked, nid, r9v>>

Ship(p, m) ==    \* a member accepts only from the primary of its own term (R4)
  /\ isPrim[p] /\ Up(p) /\ Up(m) /\ m # p /\ pos[m] = T[p] /\ T[m] = T[p]
  /\ Len(dlog[m]) < Len(ilog[p])
  /\ dlog' = [dlog EXCEPT ![m] = Append(@, ilog[p][Len(dlog[m]) + 1])]
  /\ UNCHANGED <<auth, ilog, pos, inh, pack, acked, nid, r9v>>

Apply(m) ==      \* the standby applies the next durable entry to its read state
  /\ Up(m) /\ app[m] < Len(dlog[m])
  /\ app' = [app EXCEPT ![m] = @ + 1]
  /\ UNCHANGED <<auth, logv, sl, slt, sw, fenced, nRestarts, mack>>

\* A member's ack is a message: MAck sends the prefix it reports, and p counts
\* the last one it received (mack, capped by what m still holds after a
\* truncation). So an ack sent before m was leased can be counted after.
MAck(m) ==        \* only a member of some primary's A, at that primary's term
  /\ Up(m) /\ ~isPrim[m] /\ \E p \in Nodes : isPrim[p] /\ m \in A[p] /\ T[m] = T[p]
  /\ mack' = [mack EXCEPT ![m] =
       CASE AckRule = "applied" -> app[m]
         [] AckRule = "leased"  -> IF StandbyValid(m) THEN app[m] ELSE Len(dlog[m])
         [] OTHER               -> Len(dlog[m])]
  /\ mack' # mack
  /\ UNCHANGED <<auth, logv, app, sl, slt, sw, fenced, nRestarts>>

\* what member m has acknowledged to p
Has(p, m, i) == /\ (IF m # p THEN mack[m] ELSE Len(dlog[m])) >= i
                /\ dlog[m][i] = dlog[p][i]
AckSet(p) == IF FixAckA THEN A[p] ELSE {m \in A[p] : m = p \/ m \in rec.S}

Ack(p, i) ==     \* needs valid authority now, and every member of A holding it
  /\ isPrim[p] /\ Up(p) /\ (FixAckAuth => Auth(p)) /\ i \in 1..Len(dlog[p])
  /\ \A m \in AckSet(p) : Has(p, m, i) /\ (m = p \/ T[m] = T[p])
  /\ acked' = acked \cup {<<i, dlog[p][i], T[p]>>}   \* position, entry, acking term
  /\ pack' = [pack EXCEPT ![p] = Max({@, i})]
  /\ UNCHANGED <<auth, dlog, ilog, pos, inh, nid, r9v>>

Recon(k, j) ==   \* one KIP-279 step against the current primary (R6.2)
  /\ isPrim[j] /\ Up(j) /\ Up(k) /\ k # j /\ T[k] = T[j] /\ pos[k] # T[j]
  /\ LET ts == Max({x \in Terms(ilog[j]) : x <= LastT(dlog[k])})
         L  == Min({EndLE(ilog[j], ts), EndLE(dlog[k], ts)})
     IN IF L = Len(dlog[k]) /\ ts = LastT(dlog[k])
        THEN pos' = [pos EXCEPT ![k] = T[j]] /\ UNCHANGED <<dlog, app, mack>>
        ELSE /\ dlog' = [dlog EXCEPT ![k] = SubSeq(@, 1, L)]
             /\ app' = [app EXCEPT ![k] = Min({@, L})]
             /\ mack' = [mack EXCEPT ![k] = Min({@, L})]
             /\ UNCHANGED pos
  /\ UNCHANGED <<auth, ilog, inh, pack, acked, nid, sl, slt, sw, fenced, nRestarts>>

Next ==
  \/ Tick
  \/ \E n \in Nodes : Promote(n) \/ Learn(n) \/ Ready(n) \/ Restart(n) \/ WGrant(n)
                      \/ NewEntry(n) \/ WriteLocal(n) \/ Apply(n) \/ Fence(n) \/ Unfence(n)
                      \/ MAck(n)
  \/ \E m, n \in Nodes : Grant(m, n) \/ SGrant(m, n) \/ AddA(m, n) \/ Grow(m, n)
                         \/ Shrink(m, n) \/ Drop(m, n) \/ Ship(m, n) \/ Recon(m, n)
  \/ \E p \in Nodes, i \in 1..MaxIds : Ack(p, i)

Spec == Init /\ [][Next]_vars

(* --- properties ---------------------------------------------------------- *)
\* G-30: a standby serving under a valid lease has applied every acknowledged write.
StandbyFresh == \A m \in Nodes : StandbyValid(m) =>
                  \A e \in acked : app[m] >= e[1] /\ dlog[m][e[1]] = e[2]
\* With outside-S operator promotion, writes acknowledged before it may be lost
\* by contract (G-35), so no read can reflect them. What R9 must still give:
\* a lease granted at term t reflects every write acknowledged at term t or later.
StandbyFreshOp == \A m \in Nodes : StandbyValid(m) =>
                    \A e \in acked : e[3] >= slt[m] => (app[m] >= e[1] /\ dlog[m][e[1]] = e[2])
OneAuthority == \A a, b \in Nodes : a # b => ~(Auth(a) /\ Auth(b))
AckedPreserved == \A e \in acked : \A n \in rec.S :
                    Len(dlog[n]) >= e[1] /\ dlog[n][e[1]] = e[2]
\* the same for standbys that are not (yet) primary: a mutation's counterexample
\* under this one goes through a separate standby, not the new primary's own copy
StandbyFreshNP == \A m \in Nodes : (StandbyValid(m) /\ ~isPrim[m]) =>
                    \A e \in acked : app[m] >= e[1] /\ dlog[m][e[1]] = e[2]
Safety == StandbyFresh /\ OneAuthority /\ AckedPreserved   \* in contract
SafetyOp == StandbyFreshOp /\ OneAuthority                \* with outside-S promotion

(* --- reachability probes: each must be VIOLATED, or the model is vacuous - *)
\* a valid lease while some write is acknowledged
NoLeasedRead == ~\E m \in Nodes : StandbyValid(m) /\ acked # {}
\* a valid lease granted by the second term's primary while a write is acknowledged
NoLeaseTerm2 == ~\E m \in Nodes : StandbyValid(m) /\ slt[m] = 2 /\ acked # {}
\* a valid lease survives into a state where the record is degraded
NoLeaseDegraded == ~\E m \in Nodes : StandbyValid(m) /\ rec.deg /\ m \notin rec.S
\* a leased standby has not yet applied a durable entry
NoApplyLag == ~\E m \in Nodes : StandbyValid(m) /\ app[m] < Len(dlog[m])
\* a leased standby still holds its lease after the record moved to a newer term
NoLeaseAcrossPromotion == ~\E m \in Nodes : StandbyValid(m) /\ rec.term > T[m] /\ acked # {}
=============================================================================
