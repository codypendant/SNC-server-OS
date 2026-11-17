---------------------------- MODULE PromoteRecon ----------------------------
(***************************************************************************)
(* SNC-AFR-001 cross-protocol check (Codex review of PRs #151/#152, items  *)
(* 4-6; preregistration docs/AFR_001_PROMOTION_DURING_RECON.md).          *)
(* Verification artifact, not product code.                               *)
(*                                                                         *)
(* Log.tla with R2' fixed on (S starts as all nodes, new S = S without the *)
(* old primary, positioned counting, leave S before reseed, READY after    *)
(* positioning), plus:                                                     *)
(*   - operator promotion inside S, which waives READY (R7, P2 waived);    *)
(*   - reconciliation replies in flight, tagged with the answering term;   *)
(*   - untimed standby read leases (R9). Timing is abstracted: the R3      *)
(*     barrier is "no lease of a lower term is still held anywhere", which *)
(*     is what R3 and the M2 cap guarantee (Combined.tla, StandbyLease.tla)*)
(*     and p knows exactly whether its own lease to m is still held;       *)
(*   - a history marker for the self re-promotion of a sole primary (4.2). *)
(*                                                                         *)
(* A standby's durable log stands for its applied state (R9-A, apply       *)
(* before ack, is StandbyLease.tla's concern).                             *)
(*                                                                         *)
(* Each Fix* constant switches one rule on. Turning it off must let TLC    *)
(* find the failure that rule prevents, or the run is reported as not     *)
(* discriminating.                                                         *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS Nodes, P0, MaxTerm, MaxIds, MaxCrashes,
          OpInside,      \* operator promotion inside S may waive READY (R7)
          FixQuiesce,    \* S2': a primary serves only once no truncation intent is pending
          FixDurable,    \* S2': the intent is completed durably (FALSE: cancelled in memory)
          FixRepTerm,    \* R6.3': a reply is used only at its own term, by an unpositioned non-primary
          FixBarrier,    \* R3 barrier for a self re-promoted primary (FALSE: skipped for it)
          FixLeasePos,   \* R9-B: lease only to a positioned member
          FixDropWait,   \* R9-C: no removal of m from A while p's lease to m is held
          FixStopOnLearn \* R9-D / R4: a standby stops serving when it learns a newer term

NONE == -1
NOREP == [tag |-> 0, L |-> NONE]   \* no reply in flight

VARIABLES rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks,
          acked, nid, crashes, inh, pack,
          rep,    \* reconciliation reply in flight to k: NOREP or [tag, L]
          lease,  \* standby k's read lease: 0, or the term of the primary that granted it
          cx,     \* volatile: k cancelled its pending intent in memory (FixDurable = FALSE only)
          selfT,  \* history: the term created by a self re-promotion, 0 if none
          hist    \* history: every <<lsn, entry>> any log has ever held

vars == <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes,
          inh, pack, rep, lease, cx, selfT, hist>>

Min(S) == CHOOSE x \in S : \A y \in S : x <= y
Max(S) == CHOOSE x \in S : \A y \in S : x >= y
Prefix(s, u) == Len(s) <= Len(u) /\ SubSeq(u, 1, Len(s)) = s
LastT(l) == IF Len(l) = 0 THEN 0 ELSE l[Len(l)].t
Terms(l) == {l[i].t : i \in 1..Len(l)} \cup {0}
EndLE(l, t) == Max({i \in 0..Len(l) : i = 0 \/ l[i].t <= t})

Init ==
  /\ rec = [term |-> 1, prim |-> P0, S |-> Nodes, ready |-> TRUE]
  /\ T = [n \in Nodes |-> 1]
  /\ isPrim = [n \in Nodes |-> n = P0]
  /\ dlog = [n \in Nodes |-> <<>>]
  /\ ilog = [n \in Nodes |-> <<>>]
  /\ A = [n \in Nodes |-> IF n = P0 THEN Nodes ELSE {}]
  /\ pos = [n \in Nodes |-> 1]
  /\ intent = [n \in Nodes |-> NONE]
  /\ inc = [n \in Nodes |-> FALSE]
  /\ rs = [n \in Nodes |-> FALSE]
  /\ cks = [n \in Nodes |-> <<>>]
  /\ acked = {}
  /\ nid = 0
  /\ crashes = 0
  /\ inh = [n \in Nodes |-> 0]
  /\ pack = [n \in Nodes |-> 0]
  /\ rep = [n \in Nodes |-> NOREP]
  /\ lease = [n \in Nodes |-> 0]
  /\ cx = [n \in Nodes |-> FALSE]
  /\ selfT = 0
  /\ hist = {}

Busy(n) == inc[n] \/ rs[n]
Current(p) == isPrim[p] /\ rec.prim = p /\ rec.term = T[p]
\* S2': nothing older can still change p's log. Under FixDurable = FALSE an
\* in-memory cancellation counts as done.
Quiesced(p) == FixQuiesce => (intent[p] = NONE \/ cx[p])
\* R3 barrier, untimed: no lease granted at a lower term is still held.
Barrier(p) == (FixBarrier \/ T[p] # selfT) => \A n \in Nodes : lease[n] = 0 \/ lease[n] >= T[p]
Serving(p) == isPrim[p] /\ Quiesced(p)

(* --- primary side ----------------------------------------------------- *)
NewEntry(p) ==
  /\ Serving(p) /\ nid < MaxIds
  /\ ilog' = [ilog EXCEPT ![p] = Append(@, [t |-> T[p], id |-> nid])]
  /\ nid' = nid + 1
  /\ UNCHANGED <<rec, T, isPrim, dlog, A, pos, intent, inc, rs, cks, acked, crashes, inh, pack,
                 rep, lease, cx, selfT>>

WriteLocal(p) ==   \* storage.cpp:651-652 ships before the local write
  /\ isPrim[p] /\ Len(dlog[p]) < Len(ilog[p])
  /\ dlog' = [dlog EXCEPT ![p] = Append(@, ilog[p][Len(dlog[p]) + 1])]
  /\ UNCHANGED <<rec, T, isPrim, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, cx, selfT>>

Ship(p, m) ==
  /\ Serving(p) /\ m # p /\ pos[m] = T[p] /\ T[m] = T[p] /\ ~Busy(m) /\ intent[m] = NONE
  /\ Len(dlog[m]) < Len(ilog[p])
  /\ dlog' = [dlog EXCEPT ![m] = Append(@, ilog[p][Len(dlog[m]) + 1])]
  /\ UNCHANGED <<rec, T, isPrim, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, cx, selfT>>

Ack(p, i) ==       \* every member of A, positioned in p's term (R2' rule 2)
  /\ Serving(p) /\ Barrier(p) /\ i \in 1..Len(dlog[p])
  /\ \A m \in A[p] : /\ Len(dlog[m]) >= i /\ dlog[m][i] = dlog[p][i]
                     /\ (m = p \/ (T[m] = T[p] /\ ~Busy(m) /\ pos[m] = T[p]))
  /\ acked' = acked \cup {<<i, dlog[p][i]>>}
  /\ pack' = [pack EXCEPT ![p] = Max({@, i})]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, nid, crashes, inh,
                 rep, lease, cx, selfT>>

AddA(p, c) ==
  /\ Current(p) /\ Serving(p) /\ c # p /\ c \notin A[p] /\ pos[c] = T[p] /\ T[c] = T[p] /\ ~Busy(c)
  /\ A' = [A EXCEPT ![p] = @ \cup {c}]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, cx, selfT>>

Grow(p, c) ==
  /\ Current(p) /\ Serving(p) /\ c \in A[p] /\ c \notin rec.S /\ pos[c] = T[p] /\ ~Busy(c)
  /\ Len(dlog[c]) >= Max({inh[p], pack[p]})
  /\ rec' = [rec EXCEPT !.S = @ \cup {c}]
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, cx, selfT>>

Shrink(p, c) ==
  /\ Current(p) /\ c \in rec.S /\ c # p
  /\ rec' = [rec EXCEPT !.S = @ \ {c}]
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, cx, selfT>>

Drop(p, c) ==      \* R1.2, and R9-C: not while p's lease to c is held
  /\ isPrim[p] /\ c \in A[p] /\ c # p /\ c \notin rec.S /\ rec.term = T[p]
  /\ FixDropWait => lease[c] # T[p]
  /\ A' = [A EXCEPT ![p] = @ \ {c}]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, cx, selfT>>

MarkReady(p) ==    \* R2' rule 4
  /\ Current(p) /\ Serving(p) /\ Barrier(p) /\ ~rec.ready
  /\ \A m \in rec.S \ {p} : pos[m] = T[p] /\ T[m] = T[p] /\ ~Busy(m) /\ intent[m] = NONE
  /\ rec' = [rec EXCEPT !.ready = TRUE]
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, cx, selfT>>

SGrant(p, m) ==    \* R9-B: member of A, at p's term, positioned, caught up
  /\ Current(p) /\ Serving(p) /\ Barrier(p) /\ m # p /\ m \in A[p] /\ lease[m] = 0
  /\ T[m] = T[p] /\ ~Busy(m)
  /\ FixLeasePos => (pos[m] = T[p] /\ intent[m] = NONE)
  /\ Len(dlog[m]) >= Max({inh[p], pack[p]})
  /\ lease' = [lease EXCEPT ![m] = T[p]]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh,
                 pack, rep, cx, selfT>>

LExpire(m) ==
  /\ lease[m] # 0
  /\ lease' = [lease EXCEPT ![m] = 0]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh,
                 pack, rep, cx, selfT>>

(* --- promotion -------------------------------------------------------- *)
Promote(j) ==      \* R2': j in S; new S = S \ {old primary}; or the sole member re-promotes
  /\ rec.term < MaxTerm /\ j \in rec.S /\ ~Busy(j)
  /\ \/ j # rec.prim
     \/ rec.S = {j} /\ ~isPrim[j] /\ T[j] = rec.term     \* boot read: j's term matches R (P7)
  /\ rec.ready \/ OpInside                               \* operator promotion inside S waives P2
  /\ rec' = [term |-> rec.term + 1, prim |-> j, ready |-> FALSE,
             S |-> IF j = rec.prim THEN {j} ELSE rec.S \ {rec.prim}]
  /\ selfT' = IF j = rec.prim THEN rec.term + 1 ELSE selfT
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, cx>>

Learn(n) ==        \* R4: persist the newer term first
  /\ rec.term > T[n]
  /\ T' = [T EXCEPT ![n] = rec.term]
  /\ isPrim' = [isPrim EXCEPT ![n] = (rec.prim = n)]
  /\ A' = [A EXCEPT ![n] = IF rec.prim = n THEN rec.S ELSE {}]
  /\ ilog' = [ilog EXCEPT ![n] = dlog[n]]                  \* R6.1 drain
  /\ pos' = [pos EXCEPT ![n] = IF rec.prim = n THEN rec.term ELSE 0]
  /\ inh' = [inh EXCEPT ![n] = IF rec.prim = n THEN Len(dlog[n]) ELSE 0]
  /\ pack' = [pack EXCEPT ![n] = 0]
  /\ lease' = [lease EXCEPT ![n] = IF FixStopOnLearn THEN 0 ELSE @]
  /\ cx' = [cx EXCEPT ![n] = ~FixDurable /\ rec.prim = n /\ intent[n] # NONE]
  /\ UNCHANGED <<rec, dlog, intent, inc, rs, cks, acked, nid, crashes, rep, selfT>>

(* --- reconciliation (R6.2-R6.5) --------------------------------------- *)
Answer(k, j) ==    \* KIP-279 step
  LET ts == Max({x \in Terms(ilog[j]) : x <= LastT(dlog[k])})
  IN  [ts |-> ts, L |-> Min({EndLE(ilog[j], ts), EndLE(dlog[k], ts)})]

ReconStep(k, j) == \* j answers k; a truncation answer travels as a reply tagged with j's term
  /\ Serving(j) /\ k # j /\ T[k] = T[j] /\ pos[k] # T[j] /\ ~Busy(k)
  /\ intent[k] = NONE /\ rep[k] = NOREP
  /\ LET a == Answer(k, j) IN
       IF a.L = Len(dlog[k]) /\ a.ts = LastT(dlog[k])
       THEN /\ pos' = [pos EXCEPT ![k] = T[j]]               \* positioned
            /\ UNCHANGED <<inc, rs, rep>>
       ELSE IF a.L < Len(cks[k])
            THEN /\ k \notin rec.S                           \* rule 3: leave S first
                 /\ rs' = [rs EXCEPT ![k] = TRUE]
                 /\ inc' = [inc EXCEPT ![k] = TRUE]
                 /\ UNCHANGED <<pos, rep>>
            ELSE /\ rep' = [rep EXCEPT ![k] = [tag |-> T[j], L |-> a.L]]
                 /\ UNCHANGED <<pos, inc, rs>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, intent, cks, acked, nid, crashes, inh, pack,
                 lease, cx, selfT>>

UsableRep(k) == FixRepTerm => (rep[k].tag = T[k] /\ ~isPrim[k] /\ pos[k] # T[k])

RecvRep(k) ==      \* the durable intent (R6.3) is written from the reply
  /\ rep[k] # NOREP /\ intent[k] = NONE /\ ~Busy(k) /\ UsableRep(k)
  /\ intent' = [intent EXCEPT ![k] = rep[k].L]
  /\ rep' = [rep EXCEPT ![k] = NOREP]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, inc, rs, cks, acked, nid, crashes, inh, pack,
                 lease, cx, selfT>>

DropRep(k) ==      \* a reply that cannot be used is discarded
  /\ rep[k] # NOREP /\ ~(intent[k] = NONE /\ ~Busy(k) /\ UsableRep(k))
  /\ rep' = [rep EXCEPT ![k] = NOREP]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh,
                 pack, lease, cx, selfT>>

ApplyIntent(k) ==  \* idempotent; a primary's in-memory log is truncated with it
  /\ intent[k] # NONE /\ ~cx[k] /\ Len(dlog[k]) > intent[k]
  /\ dlog' = [dlog EXCEPT ![k] = SubSeq(@, 1, intent[k])]
  /\ ilog' = [ilog EXCEPT ![k] = SubSeq(@, 1, Min({intent[k], Len(@)}))]
  /\ UNCHANGED <<rec, T, isPrim, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, cx, selfT>>

ClearIntent(k) ==  \* durable clear, only once applied (R6.3)
  /\ intent[k] # NONE /\ ~cx[k] /\ Len(dlog[k]) <= intent[k]
  /\ intent' = [intent EXCEPT ![k] = NONE]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, cx, selfT>>

Checkpoint(k) ==   \* local-only; none while an intent is pending (R6.3)
  /\ ~Busy(k) /\ Len(dlog[k]) > Len(cks[k]) /\ intent[k] = NONE
  /\ cks' = [cks EXCEPT ![k] = dlog[k]]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, acked, nid, crashes, inh, pack,
                 rep, lease, cx, selfT>>

RDiscardLog(k) ==
  /\ rs[k] /\ Len(dlog[k]) > 0
  /\ dlog' = [dlog EXCEPT ![k] = <<>>]
  /\ UNCHANGED <<rec, T, isPrim, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, cx, selfT>>

RDiscardCk(k) ==
  /\ rs[k] /\ Len(dlog[k]) = 0 /\ Len(cks[k]) > 0
  /\ cks' = [cks EXCEPT ![k] = <<>>]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, acked, nid, crashes, inh, pack,
                 rep, lease, cx, selfT>>

RCopy(k, j) ==
  /\ rs[k] /\ Serving(j) /\ T[k] = T[j] /\ Len(cks[k]) = 0 /\ Len(dlog[k]) < Len(ilog[j])
  /\ Prefix(dlog[k], ilog[j])
  /\ dlog' = [dlog EXCEPT ![k] = Append(@, ilog[j][Len(dlog[k]) + 1])]
  /\ UNCHANGED <<rec, T, isPrim, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, cx, selfT>>

RDone(k, j) ==
  /\ rs[k] /\ Serving(j) /\ T[k] = T[j] /\ Len(cks[k]) = 0 /\ Prefix(dlog[k], ilog[j])
  /\ rs' = [rs EXCEPT ![k] = FALSE]
  /\ inc' = [inc EXCEPT ![k] = FALSE]
  /\ pos' = [pos EXCEPT ![k] = T[j]]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, intent, cks, acked, nid, crashes, inh, pack,
                 rep, lease, cx, selfT>>

(* --- crash and recovery ----------------------------------------------- *)
\* R6.0: a restarted primary is no longer primary. Volatile state is lost: the
\* in-memory tail, A, positioning, the standby lease, an in-memory cancellation.
\* Recovery re-applies a pending durable intent, then clears it. A reply in
\* flight is NOT lost: it may still arrive after the restart.
Crash(n) ==
  /\ crashes < MaxCrashes
  /\ crashes' = crashes + 1
  /\ dlog' = [dlog EXCEPT ![n] = IF intent[n] # NONE
                                 THEN SubSeq(@, 1, Min({intent[n], Len(@)})) ELSE @]
  /\ ilog' = [ilog EXCEPT ![n] = dlog'[n]]
  /\ isPrim' = [isPrim EXCEPT ![n] = FALSE]
  /\ A' = [A EXCEPT ![n] = {}]
  /\ pos' = [pos EXCEPT ![n] = 0]
  /\ intent' = [intent EXCEPT ![n] = NONE]
  /\ rs' = [rs EXCEPT ![n] = inc[n]]
  /\ lease' = [lease EXCEPT ![n] = 0]
  /\ cx' = [cx EXCEPT ![n] = FALSE]
  /\ UNCHANGED <<rec, T, inc, cks, acked, nid, inh, pack, rep, selfT>>

Step ==
  \/ \E p \in Nodes : NewEntry(p) \/ WriteLocal(p) \/ Promote(p) \/ Learn(p) \/ MarkReady(p)
                      \/ ApplyIntent(p) \/ ClearIntent(p) \/ Checkpoint(p)
                      \/ RDiscardLog(p) \/ RDiscardCk(p) \/ Crash(p)
                      \/ RecvRep(p) \/ DropRep(p) \/ LExpire(p)
  \/ \E p, m \in Nodes : Ship(p, m) \/ AddA(p, m) \/ Grow(p, m) \/ Shrink(p, m)
                         \/ Drop(p, m) \/ ReconStep(m, p) \/ RCopy(m, p) \/ RDone(m, p)
                         \/ SGrant(p, m)
  \/ \E p \in Nodes, i \in 1..MaxIds : Ack(p, i)

Held(l) == UNION {{<<i, l[n][i]>> : i \in 1..Len(l[n])} : n \in Nodes}
Next == Step /\ hist' = hist \cup Held(dlog') \cup Held(ilog')

Spec == Init /\ [][Next]_vars

(* --- properties ------------------------------------------------------- *)
HoldsAcked(n) == \A e \in acked : Len(dlog[n]) >= e[1] /\ dlog[n][e[1]] = e[2]
\* I4: every acknowledged entry is held by every member of the current S
AckedPreserved == \A n \in rec.S : HoldsAcked(n)
EntryUnique == \A a, b \in Nodes : \A i \in 1..Min({Len(dlog[a]), Len(dlog[b])}) :
                 dlog[a][i].t = dlog[b][i].t => dlog[a][i] = dlog[b][i]
HistoryUnique == \A x, y \in hist : (x[1] = y[1] /\ x[2].t = y[2].t) => x[2] = y[2]
LogMatching == \A a, b \in Nodes : \A i \in 1..Min({Len(dlog[a]), Len(dlog[b])}) :
                 dlog[a][i] = dlog[b][i] => SubSeq(dlog[a], 1, i) = SubSeq(dlog[b], 1, i)
StateMatchesLog == \A n \in Nodes : ~Busy(n) => Prefix(cks[n], dlog[n])
PositionedPrefix == \A k, j \in Nodes :
                      (isPrim[j] /\ k # j /\ pos[k] = T[j] /\ T[k] = T[j] /\ ~Busy(k)
                       /\ intent[k] = NONE)
                      => Prefix(dlog[k], ilog[j])
\* G-30: a standby with a lease reflects every acknowledged write
StandbyFresh == \A m \in Nodes : lease[m] # 0 => HoldsAcked(m)
\* Stronger than G-30: while the granting primary is current, a leased standby
\* serves a prefix of that primary's log (no divergent tail from an older term).
LeasedPrefix == \A m, p \in Nodes :
                  (lease[m] # 0 /\ isPrim[p] /\ T[p] = lease[m] /\ m # p) => Prefix(dlog[m], ilog[p])
Safety == AckedPreserved /\ EntryUnique /\ LogMatching /\ StateMatchesLog /\ PositionedPrefix
          /\ HistoryUnique /\ StandbyFresh /\ LeasedPrefix
\* Reachability probe: TLC must find a state where it fails.
NoSelfRepromotion == selfT = 0
=============================================================================
