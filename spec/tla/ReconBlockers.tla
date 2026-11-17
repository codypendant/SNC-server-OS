---------------------------- MODULE ReconBlockers ----------------------------
(***************************************************************************)
(* SNC-AFR-001 follow-up to PR #153 (Codex review, docs/handoff/           *)
(* PR153_Codex_Review.md; preregistration docs/AFR_001_RECON_BLOCKERS.md). *)
(* Verification artifact, not product code.                               *)
(*                                                                         *)
(* PromoteRecon.tla with every rule #153 showed necessary hard-wired on    *)
(* (quiescence with durable completion, the reply term check, the R6.4     *)
(* re-check, the self re-promotion barrier, R9-B/C/D), and three changes:  *)
(*   1. replies form a set per node: a reply can be delivered any number   *)
(*      of times, in any order, or lost, before or after a crash. Every    *)
(*      answer, including "positioned", travels as a reply. A reply        *)
(*      carries the snapshot (last term, end LSN) of the log it answered;  *)
(*      FixBind is R6.3' item 1's binding check;                           *)
(*   2. FixIntentBlock is R6.3' item 3: a node with a pending intent sends *)
(*      no request, is not positioned, is not shipped to, does not count   *)
(*      toward an acknowledgement, READY, A or S, and holds no lease;      *)
(*   3. FixNewTerm: a sole primary that restarts serves again only through *)
(*      a new witness-approved term (4.2). FALSE adds Resume, which lets   *)
(*      it serve again at its old term.                                    *)
(*                                                                         *)
(* The binding is the log snapshot, not an incarnation and request number. *)
(* The preregistration (section 2) argues why that is equivalent here.     *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS Nodes, P0, MaxTerm, MaxIds, MaxCrashes, MaxReps,
          OpInside,       \* operator promotion inside S may waive READY (R7)
          FixBind,        \* R6.3' item 1: a reply is used only for the log it answered
          FixIntentBlock, \* R6.3' item 3: a pending intent blocks the node in every term
          FixNewTerm,     \* 4.2: a restarted sole primary needs a new term (FALSE adds Resume)
          Ckpt,           \* scope: checkpoints (and so reseeding) are modeled; FALSE only to bound a lane
          Leases          \* scope: standby read leases are modeled; FALSE only in a fallback lane

NONE == -1
NOINT == [L |-> NONE, t |-> 0]

VARIABLES rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks,
          acked, nid, crashes, inh, pack,
          rep,    \* replies to k: a set of [tag, L, ts, blt, ble]
          lease,  \* standby k's read lease: 0, or the term of the primary that granted it
          selfT,  \* history: the term created by a self re-promotion, 0 if none
          hist    \* history: every <<lsn, entry>> any log has ever held

vars == <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes,
          inh, pack, rep, lease, selfT, hist>>

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
  /\ intent = [n \in Nodes |-> NOINT]
  /\ inc = [n \in Nodes |-> FALSE]
  /\ rs = [n \in Nodes |-> FALSE]
  /\ cks = [n \in Nodes |-> <<>>]
  /\ acked = {}
  /\ nid = 0
  /\ crashes = 0
  /\ inh = [n \in Nodes |-> 0]
  /\ pack = [n \in Nodes |-> 0]
  /\ rep = [n \in Nodes |-> {}]
  /\ lease = [n \in Nodes |-> 0]
  /\ selfT = 0
  /\ hist = {}

Busy(n) == inc[n] \/ rs[n]
Pending(n) == intent[n] # NOINT
Free(n) == FixIntentBlock => ~Pending(n)        \* R6.3' item 3
Current(p) == isPrim[p] /\ rec.prim = p /\ rec.term = T[p]
Serving(p) == isPrim[p] /\ ~Pending(p)          \* S2' quiescence, durable completion
\* R3 barrier, untimed: no lease granted at a lower term is still held.
Barrier(p) == \A n \in Nodes : lease[n] = 0 \/ lease[n] >= T[p]
\* m follows p's log in p's term and may count for it
Follows(m, p) == pos[m] = T[p] /\ T[m] = T[p] /\ ~Busy(m) /\ Free(m)

(* --- primary side ----------------------------------------------------- *)
NewEntry(p) ==
  /\ Serving(p) /\ nid < MaxIds
  /\ ilog' = [ilog EXCEPT ![p] = Append(@, [t |-> T[p], id |-> nid])]
  /\ nid' = nid + 1
  /\ UNCHANGED <<rec, T, isPrim, dlog, A, pos, intent, inc, rs, cks, acked, crashes, inh, pack,
                 rep, lease, selfT>>

WriteLocal(p) ==   \* storage.cpp:651-652 ships before the local write
  /\ isPrim[p] /\ Len(dlog[p]) < Len(ilog[p])
  /\ dlog' = [dlog EXCEPT ![p] = Append(@, ilog[p][Len(dlog[p]) + 1])]
  /\ UNCHANGED <<rec, T, isPrim, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, selfT>>

Ship(p, m) ==
  /\ Serving(p) /\ m # p /\ Follows(m, p)
  /\ Len(dlog[m]) < Len(ilog[p])
  /\ dlog' = [dlog EXCEPT ![m] = Append(@, ilog[p][Len(dlog[m]) + 1])]
  /\ UNCHANGED <<rec, T, isPrim, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, selfT>>

Ack(p, i) ==       \* every member of A, positioned in p's term (R2' rule 2)
  /\ Serving(p) /\ Barrier(p) /\ i \in 1..Len(dlog[p])
  /\ \A m \in A[p] : /\ Len(dlog[m]) >= i /\ dlog[m][i] = dlog[p][i]
                     /\ (m = p \/ Follows(m, p))
  /\ acked' = acked \cup {<<i, dlog[p][i]>>}
  /\ pack' = [pack EXCEPT ![p] = Max({@, i})]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, nid, crashes, inh,
                 rep, lease, selfT>>

AddA(p, c) ==
  /\ Current(p) /\ Serving(p) /\ c # p /\ c \notin A[p] /\ Follows(c, p)
  /\ A' = [A EXCEPT ![p] = @ \cup {c}]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, selfT>>

Grow(p, c) ==
  /\ Current(p) /\ Serving(p) /\ c \in A[p] /\ c \notin rec.S /\ Follows(c, p)
  /\ Len(dlog[c]) >= Max({inh[p], pack[p]})
  /\ rec' = [rec EXCEPT !.S = @ \cup {c}]
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, selfT>>

Shrink(p, c) ==
  /\ Current(p) /\ c \in rec.S /\ c # p
  /\ rec' = [rec EXCEPT !.S = @ \ {c}]
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, selfT>>

Drop(p, c) ==      \* R1.2, and R9-C: not while p's lease to c is held
  /\ isPrim[p] /\ c \in A[p] /\ c # p /\ c \notin rec.S /\ rec.term = T[p]
  /\ lease[c] # T[p]
  /\ A' = [A EXCEPT ![p] = @ \ {c}]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, selfT>>

MarkReady(p) ==    \* R2' rule 4
  /\ Current(p) /\ Serving(p) /\ Barrier(p) /\ ~rec.ready
  /\ \A m \in rec.S \ {p} : Follows(m, p)
  /\ rec' = [rec EXCEPT !.ready = TRUE]
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, selfT>>

SGrant(p, m) ==    \* R9-B: member of A, at p's term, positioned, no pending intent, caught up
  /\ Leases /\ Current(p) /\ Serving(p) /\ Barrier(p) /\ m # p /\ m \in A[p] /\ lease[m] = 0
  /\ Follows(m, p)
  /\ Len(dlog[m]) >= Max({inh[p], pack[p]})
  /\ lease' = [lease EXCEPT ![m] = T[p]]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh,
                 pack, rep, selfT>>

LExpire(m) ==
  /\ lease[m] # 0
  /\ lease' = [lease EXCEPT ![m] = 0]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh,
                 pack, rep, selfT>>

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
                 rep, lease>>

\* Mutation only (FixNewTerm = FALSE): the restarted sole primary serves again
\* at its old term, without a witness-approved promotion.
Resume(p) ==
  /\ ~FixNewTerm
  /\ ~isPrim[p] /\ rec.prim = p /\ rec.S = {p} /\ T[p] = rec.term /\ ~Busy(p)
  /\ isPrim' = [isPrim EXCEPT ![p] = TRUE]
  /\ A' = [A EXCEPT ![p] = {p}]
  /\ pos' = [pos EXCEPT ![p] = T[p]]
  /\ inh' = [inh EXCEPT ![p] = Len(dlog[p])]
  /\ pack' = [pack EXCEPT ![p] = 0]
  /\ UNCHANGED <<rec, T, dlog, ilog, intent, inc, rs, cks, acked, nid, crashes, rep, lease, selfT>>

Learn(n) ==        \* R4: persist the newer term first; a pending intent stays pending
  /\ rec.term > T[n]
  /\ T' = [T EXCEPT ![n] = rec.term]
  /\ isPrim' = [isPrim EXCEPT ![n] = (rec.prim = n)]
  /\ A' = [A EXCEPT ![n] = IF rec.prim = n THEN rec.S ELSE {}]
  /\ ilog' = [ilog EXCEPT ![n] = dlog[n]]                  \* R6.1 drain
  /\ pos' = [pos EXCEPT ![n] = IF rec.prim = n THEN rec.term ELSE 0]
  /\ inh' = [inh EXCEPT ![n] = IF rec.prim = n THEN Len(dlog[n]) ELSE 0]
  /\ pack' = [pack EXCEPT ![n] = 0]
  /\ lease' = [lease EXCEPT ![n] = 0]                      \* R9-D
  /\ UNCHANGED <<rec, dlog, intent, inc, rs, cks, acked, nid, crashes, rep, selfT>>

(* --- reconciliation (R6.2-R6.5) --------------------------------------- *)
Answer(k, j) ==    \* KIP-279 step
  LET ts == Max({x \in Terms(ilog[j]) : x <= LastT(dlog[k])})
  IN  [ts |-> ts, L |-> Min({EndLE(ilog[j], ts), EndLE(dlog[k], ts)})]

ReconStep(k, j) == \* k asks, j answers; the reply joins k's reply set
  /\ Serving(j) /\ k # j /\ T[k] = T[j] /\ pos[k] # T[j] /\ ~Busy(k) /\ Free(k)
  /\ Cardinality(rep[k]) < MaxReps
  /\ LET a == Answer(k, j) IN
       rep' = [rep EXCEPT ![k] = @ \cup {[tag |-> T[j], L |-> a.L, ts |-> a.ts,
                                         blt |-> LastT(dlog[k]), ble |-> Len(dlog[k])]}]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh,
                 pack, lease, selfT>>

\* R6.3': the reply is at k's own term, k is an unpositioned non-primary, and
\* (FixBind) k's log is still the one the reply answered.
Usable(k, r) == /\ r.tag = T[k] /\ ~isPrim[k] /\ pos[k] # T[k] /\ ~Busy(k)
                /\ FixBind => (r.blt = LastT(dlog[k]) /\ r.ble = Len(dlog[k]))

RecvRep(k, r) ==   \* delivery does not consume the reply: it may come again
  /\ r \in rep[k] /\ Usable(k, r) /\ Free(k)
  /\ IF r.L = Len(dlog[k]) /\ r.ts = LastT(dlog[k])
     THEN /\ pos' = [pos EXCEPT ![k] = T[k]]                 \* positioned
          /\ UNCHANGED <<intent, inc, rs>>
     ELSE IF r.L < Len(cks[k])
          THEN /\ k \notin rec.S                             \* rule 3: leave S first
               /\ rs' = [rs EXCEPT ![k] = TRUE]
               /\ inc' = [inc EXCEPT ![k] = TRUE]
               /\ UNCHANGED <<pos, intent>>
          ELSE /\ ~Pending(k)                                \* R6.4 re-checked above
               /\ intent' = [intent EXCEPT ![k] = [L |-> r.L, t |-> T[k]]]
               /\ UNCHANGED <<pos, inc, rs>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, cks, acked, nid, crashes, inh, pack, rep,
                 lease, selfT>>

DropRep(k, r) ==   \* loss
  /\ r \in rep[k]
  /\ rep' = [rep EXCEPT ![k] = @ \ {r}]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh,
                 pack, lease, selfT>>

ApplyIntent(k) ==  \* idempotent; a primary's in-memory log is truncated with it
  /\ Pending(k) /\ Len(dlog[k]) > intent[k].L
  /\ dlog' = [dlog EXCEPT ![k] = SubSeq(@, 1, intent[k].L)]
  /\ ilog' = [ilog EXCEPT ![k] = SubSeq(@, 1, Min({intent[k].L, Len(@)}))]
  /\ UNCHANGED <<rec, T, isPrim, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, selfT>>

ClearIntent(k) ==  \* durable clear, only once applied (R6.3)
  /\ Pending(k) /\ Len(dlog[k]) <= intent[k].L
  /\ intent' = [intent EXCEPT ![k] = NOINT]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, selfT>>

Checkpoint(k) ==   \* local-only; none while an intent is pending (R6.3)
  /\ Ckpt /\ ~Busy(k) /\ Len(dlog[k]) > Len(cks[k]) /\ ~Pending(k)
  /\ cks' = [cks EXCEPT ![k] = dlog[k]]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, acked, nid, crashes, inh, pack,
                 rep, lease, selfT>>

RDiscardLog(k) ==
  /\ rs[k] /\ Len(dlog[k]) > 0
  /\ dlog' = [dlog EXCEPT ![k] = <<>>]
  /\ UNCHANGED <<rec, T, isPrim, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, selfT>>

RDiscardCk(k) ==
  /\ rs[k] /\ Len(dlog[k]) = 0 /\ Len(cks[k]) > 0
  /\ cks' = [cks EXCEPT ![k] = <<>>]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, acked, nid, crashes, inh, pack,
                 rep, lease, selfT>>

RCopy(k, j) ==
  /\ rs[k] /\ Serving(j) /\ T[k] = T[j] /\ Len(cks[k]) = 0 /\ Len(dlog[k]) < Len(ilog[j])
  /\ Prefix(dlog[k], ilog[j])
  /\ dlog' = [dlog EXCEPT ![k] = Append(@, ilog[j][Len(dlog[k]) + 1])]
  /\ UNCHANGED <<rec, T, isPrim, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack,
                 rep, lease, selfT>>

RDone(k, j) ==
  /\ rs[k] /\ Serving(j) /\ T[k] = T[j] /\ Len(cks[k]) = 0 /\ Prefix(dlog[k], ilog[j])
  /\ rs' = [rs EXCEPT ![k] = FALSE]
  /\ inc' = [inc EXCEPT ![k] = FALSE]
  /\ pos' = [pos EXCEPT ![k] = T[j]]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, intent, cks, acked, nid, crashes, inh, pack,
                 rep, lease, selfT>>

(* --- crash and recovery ----------------------------------------------- *)
\* R6.0: a restarted primary is no longer primary. Volatile state is lost: the
\* in-memory tail, A, positioning, the standby lease. Recovery completes a
\* pending durable intent, then clears it. Replies already sent survive the
\* restart and may still be delivered.
Crash(n) ==
  /\ crashes < MaxCrashes
  /\ crashes' = crashes + 1
  /\ dlog' = [dlog EXCEPT ![n] = IF Pending(n)
                                 THEN SubSeq(@, 1, Min({intent[n].L, Len(@)})) ELSE @]
  /\ ilog' = [ilog EXCEPT ![n] = dlog'[n]]
  /\ isPrim' = [isPrim EXCEPT ![n] = FALSE]
  /\ A' = [A EXCEPT ![n] = {}]
  /\ pos' = [pos EXCEPT ![n] = 0]
  /\ intent' = [intent EXCEPT ![n] = NOINT]
  /\ rs' = [rs EXCEPT ![n] = inc[n]]
  /\ lease' = [lease EXCEPT ![n] = 0]
  /\ UNCHANGED <<rec, T, inc, cks, acked, nid, inh, pack, rep, selfT>>

Step ==
  \/ \E p \in Nodes : NewEntry(p) \/ WriteLocal(p) \/ Promote(p) \/ Learn(p) \/ MarkReady(p)
                      \/ ApplyIntent(p) \/ ClearIntent(p) \/ Checkpoint(p)
                      \/ RDiscardLog(p) \/ RDiscardCk(p) \/ Crash(p) \/ Resume(p) \/ LExpire(p)
  \/ \E p, m \in Nodes : Ship(p, m) \/ AddA(p, m) \/ Grow(p, m) \/ Shrink(p, m)
                         \/ Drop(p, m) \/ ReconStep(m, p) \/ RCopy(m, p) \/ RDone(m, p)
                         \/ SGrant(p, m)
  \/ \E p \in Nodes, i \in 1..MaxIds : Ack(p, i)
  \/ \E k \in Nodes : \E r \in rep[k] : RecvRep(k, r) \/ DropRep(k, r)

Held(l) == UNION {{<<i, l[n][i]>> : i \in 1..Len(l[n])} : n \in Nodes}
Next == Step /\ hist' = hist \cup Held(dlog') \cup Held(ilog')

Spec == Init /\ [][Next]_vars
\* Nodes other than P0 are interchangeable (used for the four-node lane only)
Sym == Permutations(Nodes \ {P0})

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
                      (isPrim[j] /\ k # j /\ pos[k] = T[j] /\ T[k] = T[j] /\ ~Busy(k) /\ ~Pending(k))
                      => Prefix(dlog[k], ilog[j])
StandbyFresh == \A m \in Nodes : lease[m] # 0 => HoldsAcked(m)
LeasedPrefix == \A m, p \in Nodes :
                  (lease[m] # 0 /\ isPrim[p] /\ T[p] = lease[m] /\ m # p) => Prefix(dlog[m], ilog[p])
Safety == AckedPreserved /\ EntryUnique /\ LogMatching /\ StateMatchesLog /\ PositionedPrefix
          /\ HistoryUnique /\ StandbyFresh /\ LeasedPrefix

(* --- reachability probes: TLC must report each one violated ---------- *)
NoSelfRepromotion == selfT = 0
\* a self re-promoted term reuses an LSN that an older term's entry held,
\* so HistoryUnique is checked across that reuse, not vacuously
NoLsnReuse == ~\E x, y \in hist : x[1] = y[1] /\ x[2].t # y[2].t /\ selfT \in {x[2].t, y[2].t}
\* a node carries an intent written at an older term into a newer term
NoCrossTermIntent == \A k \in Nodes : ~Pending(k) \/ intent[k].t = T[k]
=============================================================================
