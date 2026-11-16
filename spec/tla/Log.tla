------------------------------- MODULE Log -------------------------------
(***************************************************************************)
(* SNC-AFR-001 Claim D: log prefix and acknowledged-write preservation.   *)
(* Verification artifact for docs/AFR_001_SAFETY_ANALYSIS.md: R1, R2,     *)
(* R6, I1-I5. Not product code.                                            *)
(*                                                                         *)
(* Authority is assumed, not modelled here: Authority.tla covers Claim L. *)
(* A member acknowledges only for its own term (R4), so a primary that has *)
(* been replaced cannot complete an acknowledgement once A has moved on.   *)
(*                                                                         *)
(* An entry is a record [t |-> term, id |-> unique id]. The id lets TLC    *)
(* see two different entries that share (term, lsn).                       *)
(*                                                                         *)
(* Each Fix* constant switches one fix on. Turning a fix off must let TLC  *)
(* find the failure that fix prevents.                                     *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS Nodes, P0, Q0, MaxTerm, MaxIds, MaxCrashes,
          FixB1,      \* R1.2: drop from A only once outside S
          FixR60,     \* R6.0: a restarted primary never writes its old term again
          FixM1,      \* R6.2: KIP-279 iteration (FALSE: KIP-101 single answer)
          FixClear,   \* R6.3: clear the intent before accepting entries beyond it
          FixReseed,  \* R6.4: reseed when the truncation point is below the checkpoint
          FixInc,     \* R6.5: durable incomplete flag during a reseed
          FixCkIntent,\* R6.3 (found by TLC): no checkpoint while an intent is pending
          KeepS       \* relax R2 Kafka-style: promotion keeps the rest of S, unreconciled
                      \* (FALSE is the protocol; TRUE exists to exercise R6.2, see RESULTS)

NONE == -1

VARIABLES rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks,
          acked, nid, crashes,
          inh,   \* primary-local: its log end when it learned its promotion
          pack,  \* primary-local: highest LSN it acknowledged in its own term
          hist   \* history only: every <<lsn, entry>> any log has ever held

vars == <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks,
          acked, nid, crashes, inh, pack, hist>>

Min(S) == CHOOSE x \in S : \A y \in S : x <= y
Max(S) == CHOOSE x \in S : \A y \in S : x >= y
Prefix(s, u) == Len(s) <= Len(u) /\ SubSeq(u, 1, Len(s)) = s
LastT(l) == IF Len(l) = 0 THEN 0 ELSE l[Len(l)].t
Terms(l) == {l[i].t : i \in 1..Len(l)} \cup {0}
EndLE(l, t) == Max({i \in 0..Len(l) : i = 0 \/ l[i].t <= t})

Init ==
  /\ rec = [term |-> 1, prim |-> P0, S |-> {P0, Q0}]
  /\ T = [n \in Nodes |-> 1]
  /\ isPrim = [n \in Nodes |-> n = P0]
  /\ dlog = [n \in Nodes |-> <<>>]
  /\ ilog = [n \in Nodes |-> <<>>]
  /\ A = [n \in Nodes |-> IF n = P0 THEN {P0, Q0} ELSE {}]
  /\ pos = [n \in Nodes |-> IF n \in {P0, Q0} THEN 1 ELSE 0]
  /\ intent = [n \in Nodes |-> NONE]
  /\ inc = [n \in Nodes |-> FALSE]
  /\ rs = [n \in Nodes |-> FALSE]
  /\ cks = [n \in Nodes |-> <<>>]
  /\ acked = {}
  /\ nid = 0
  /\ crashes = 0
  /\ inh = [n \in Nodes |-> 0]
  /\ pack = [n \in Nodes |-> 0]
  /\ hist = {}

Busy(n) == inc[n] \/ rs[n]
IntentDone(n) == intent[n] = NONE \/ (~FixClear /\ Len(dlog[n]) <= intent[n])

(* --- primary side ----------------------------------------------------- *)
NewEntry(p) ==
  /\ isPrim[p] /\ nid < MaxIds
  /\ ilog' = [ilog EXCEPT ![p] = Append(@, [t |-> T[p], id |-> nid])]
  /\ nid' = nid + 1
  /\ UNCHANGED <<rec, T, isPrim, dlog, A, pos, intent, inc, rs, cks, acked, crashes, inh, pack>>

WriteLocal(p) ==   \* storage.cpp:651-652 ships before the local write
  /\ isPrim[p] /\ Len(dlog[p]) < Len(ilog[p])
  /\ dlog' = [dlog EXCEPT ![p] = Append(@, ilog[p][Len(dlog[p]) + 1])]
  /\ UNCHANGED <<rec, T, isPrim, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack>>

Ship(p, m) ==
  /\ isPrim[p] /\ m # p /\ pos[m] = T[p] /\ T[m] = T[p] /\ ~Busy(m) /\ IntentDone(m)
  /\ Len(dlog[m]) < Len(ilog[p])
  /\ dlog' = [dlog EXCEPT ![m] = Append(@, ilog[p][Len(dlog[m]) + 1])]
  /\ UNCHANGED <<rec, T, isPrim, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack>>

Ack(p, i) ==
  /\ isPrim[p] /\ i \in 1..Len(dlog[p])
  /\ \A m \in A[p] : /\ Len(dlog[m]) >= i /\ dlog[m][i] = dlog[p][i]
                     /\ (m = p \/ (T[m] = T[p] /\ ~Busy(m)))
  /\ acked' = acked \cup {<<i, dlog[p][i]>>}
  /\ pack' = [pack EXCEPT ![p] = Max({@, i})]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, nid, crashes, inh>>

Current(p) == isPrim[p] /\ rec.prim = p /\ rec.term = T[p]

AddA(p, c) ==
  /\ Current(p) /\ c # p /\ c \notin A[p] /\ pos[c] = T[p] /\ T[c] = T[p] /\ ~Busy(c)
  /\ A' = [A EXCEPT ![p] = @ \cup {c}]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack>>

Grow(p, c) ==      \* "caught up" (section 2): positioned, and durable end >=
                   \* max(p's inherited log end, p's own acknowledged LSN)
  /\ Current(p) /\ c \in A[p] /\ c \notin rec.S /\ pos[c] = T[p] /\ ~Busy(c)
  /\ Len(dlog[c]) >= Max({inh[p], pack[p]})
  /\ rec' = [rec EXCEPT !.S = @ \cup {c}]
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack>>

Shrink(p, c) ==
  /\ Current(p) /\ c \in rec.S /\ c # p
  /\ rec' = [rec EXCEPT !.S = @ \ {c}]
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack>>

Drop(p, c) ==      \* without FixB1: a lost grow reply is treated as a failure
  /\ isPrim[p] /\ c \in A[p] /\ c # p
  /\ (FixB1 => (c \notin rec.S /\ rec.term = T[p]))   \* read of R still at p's term
  /\ A' = [A EXCEPT ![p] = @ \ {c}]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack>>

Promote(j) ==      \* R2: j in S; new S = {j}
  /\ rec.term < MaxTerm /\ j \in rec.S /\ j # rec.prim /\ ~Busy(j)
  /\ rec' = [term |-> rec.term + 1, prim |-> j, S |-> IF KeepS THEN rec.S ELSE {j}]
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack>>

Learn(n) ==
  /\ rec.term > T[n]
  /\ T' = [T EXCEPT ![n] = rec.term]
  /\ isPrim' = [isPrim EXCEPT ![n] = (rec.prim = n)]
  /\ A' = [A EXCEPT ![n] = IF rec.prim = n THEN (IF KeepS THEN rec.S ELSE {n}) ELSE {}]
  /\ ilog' = [ilog EXCEPT ![n] = dlog[n]]                  \* R6.1 drain
  /\ pos' = [pos EXCEPT ![n] = IF rec.prim = n THEN rec.term ELSE 0]
  /\ inh' = [inh EXCEPT ![n] = IF rec.prim = n THEN Len(dlog[n]) ELSE 0]
  /\ pack' = [pack EXCEPT ![n] = 0]
  /\ UNCHANGED <<rec, dlog, intent, inc, rs, cks, acked, nid, crashes>>

(* --- reconciliation (R6.2-R6.5) --------------------------------------- *)
\* Truncation point that k's next reconciliation step would choose against j.
Answer(k, j) ==
  IF FixM1
  THEN LET ts == Max({x \in Terms(ilog[j]) : x <= LastT(dlog[k])})
       IN  [ts |-> ts, L |-> Min({EndLE(ilog[j], ts), EndLE(dlog[k], ts)})]
  ELSE \* KIP-101: end of k's last term in j if j has it, else start of the next term - 1
       LET tk == LastT(dlog[k])
           nxt == {i \in 1..Len(ilog[j]) : ilog[j][i].t > tk}
           L   == IF nxt = {} THEN Len(ilog[j]) ELSE Min(nxt) - 1
       IN  [ts |-> tk, L |-> Min({L, Len(dlog[k])})]

ReconStep(k, j) ==
  /\ isPrim[j] /\ k # j /\ T[k] = T[j] /\ pos[k] # T[j] /\ ~Busy(k) /\ IntentDone(k)
  /\ LET a == Answer(k, j) IN
       IF a.L = Len(dlog[k]) /\ (FixM1 => a.ts = LastT(dlog[k]))
       THEN /\ pos' = [pos EXCEPT ![k] = T[j]]               \* positioned
            /\ UNCHANGED <<intent, inc, rs, inh, pack>>
       ELSE IF FixReseed /\ a.L < Len(cks[k])
            THEN /\ rs' = [rs EXCEPT ![k] = TRUE]            \* start a reseed
                 /\ inc' = [inc EXCEPT ![k] = FixInc]
                 /\ UNCHANGED <<pos, intent, inh, pack>>
            ELSE /\ intent' = [intent EXCEPT ![k] = a.L]     \* durable intent
                 /\ UNCHANGED <<pos, inc, rs, inh, pack>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, cks, acked, nid, crashes, inh, pack>>

ApplyIntent(k) ==
  /\ intent[k] # NONE /\ Len(dlog[k]) > intent[k]
  /\ dlog' = [dlog EXCEPT ![k] = SubSeq(@, 1, intent[k])]
  /\ UNCHANGED <<rec, T, isPrim, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack>>

ClearIntent(k) ==
  /\ intent[k] # NONE /\ Len(dlog[k]) <= intent[k]
  /\ intent' = [intent EXCEPT ![k] = NONE]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, inc, rs, cks, acked, nid, crashes, inh, pack>>

Checkpoint(k) ==   \* checkpoints are local-only (shard.cpp:2710-2716)
  /\ ~Busy(k) /\ Len(dlog[k]) > Len(cks[k])
  /\ (FixCkIntent => intent[k] = NONE)
  /\ cks' = [cks EXCEPT ![k] = dlog[k]]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, acked, nid, crashes, inh, pack>>

RDiscardLog(k) ==
  /\ rs[k] /\ Len(dlog[k]) > 0
  /\ dlog' = [dlog EXCEPT ![k] = <<>>]
  /\ UNCHANGED <<rec, T, isPrim, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack>>

RDiscardCk(k) ==
  /\ rs[k] /\ Len(dlog[k]) = 0 /\ Len(cks[k]) > 0
  /\ cks' = [cks EXCEPT ![k] = <<>>]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, inc, rs, acked, nid, crashes, inh, pack>>

RCopy(k, j) ==
  /\ rs[k] /\ isPrim[j] /\ T[k] = T[j] /\ Len(cks[k]) = 0 /\ Len(dlog[k]) < Len(ilog[j])
  /\ Prefix(dlog[k], ilog[j])
  /\ dlog' = [dlog EXCEPT ![k] = Append(@, ilog[j][Len(dlog[k]) + 1])]
  /\ UNCHANGED <<rec, T, isPrim, ilog, A, pos, intent, inc, rs, cks, acked, nid, crashes, inh, pack>>

RDone(k, j) ==
  /\ rs[k] /\ isPrim[j] /\ T[k] = T[j] /\ Len(cks[k]) = 0 /\ Prefix(dlog[k], ilog[j])
  /\ rs' = [rs EXCEPT ![k] = FALSE]
  /\ inc' = [inc EXCEPT ![k] = FALSE]
  /\ pos' = [pos EXCEPT ![k] = T[j]]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, intent, cks, acked, nid, crashes, inh, pack>>

(* --- crash and recovery ----------------------------------------------- *)
Crash(n) ==
  /\ crashes < MaxCrashes
  /\ crashes' = crashes + 1
  /\ ilog' = [ilog EXCEPT ![n] = dlog[n]]                  \* volatile tail lost
  /\ isPrim' = [isPrim EXCEPT ![n] = IF FixR60 THEN FALSE ELSE @]
  /\ A' = [A EXCEPT ![n] = IF FixR60 THEN {} ELSE @]
  /\ pos' = [pos EXCEPT ![n] = IF ~FixR60 /\ isPrim[n] THEN @ ELSE 0]
  \* recovery re-applies a pending intent, then clears it
  /\ dlog' = [dlog EXCEPT ![n] = IF intent[n] # NONE
                                 THEN SubSeq(@, 1, Min({intent[n], Len(@)})) ELSE @]
  /\ intent' = [intent EXCEPT ![n] = NONE]
  /\ rs' = [rs EXCEPT ![n] = inc[n]]                       \* resume a flagged reseed
  /\ UNCHANGED <<rec, T, inc, cks, acked, nid, inh, pack>>

Step ==
  \/ \E p \in Nodes : NewEntry(p) \/ WriteLocal(p) \/ Promote(p) \/ Learn(p)
                      \/ ApplyIntent(p) \/ ClearIntent(p) \/ Checkpoint(p)
                      \/ RDiscardLog(p) \/ RDiscardCk(p) \/ Crash(p)
  \/ \E p, m \in Nodes : Ship(p, m) \/ AddA(p, m) \/ Grow(p, m) \/ Shrink(p, m)
                         \/ Drop(p, m) \/ ReconStep(m, p) \/ RCopy(m, p) \/ RDone(m, p)
  \/ \E p \in Nodes, i \in 1..MaxIds : Ack(p, i)

\* hist records every entry any durable or in-memory log has ever held at each LSN.
\* It is written only here and read only by HistoryUnique, so it adds no behaviour to
\* the other variables. It does depend on the path taken, so TLC counts more distinct
\* states: state counts with hist are not comparable with earlier runs.
Held(l) == UNION {{<<i, l[n][i]>> : i \in 1..Len(l[n])} : n \in Nodes}
Next == Step /\ hist' = hist \cup Held(dlog') \cup Held(ilog')

Spec == Init /\ [][Next]_vars

(* --- properties ------------------------------------------------------- *)
\* I4: every acknowledged entry is held by every member of the current S
AckedPreserved == \A e \in acked : \A n \in rec.S :
                    Len(dlog[n]) >= e[1] /\ dlog[n][e[1]] = e[2]
\* I1: same (term, lsn) means the same entry
EntryUnique == \A a, b \in Nodes : \A i \in 1..Min({Len(dlog[a]), Len(dlog[b])}) :
                 dlog[a][i].t = dlog[b][i].t => dlog[a][i] = dlog[b][i]
\* G-15 over history: a (term, lsn) pair never identifies two different entries,
\* even when one of them no longer exists anywhere. An LSN may be reused under another term.
HistoryUnique == \A x, y \in hist : (x[1] = y[1] /\ x[2].t = y[2].t) => x[2] = y[2]
\* I2: same entry at i means the same prefix up to i
LogMatching == \A a, b \in Nodes : \A i \in 1..Min({Len(dlog[a]), Len(dlog[b])}) :
                 dlog[a][i] = dlog[b][i] => SubSeq(dlog[a], 1, i) = SubSeq(dlog[b], 1, i)
\* I5: applied (checkpointed) state always matches the log, unless flagged
StateMatchesLog == \A n \in Nodes : ~Busy(n) => Prefix(cks[n], dlog[n])
\* positioned members hold a prefix of their primary's log
PositionedPrefix == \A k, j \in Nodes :
                      (isPrim[j] /\ k # j /\ pos[k] = T[j] /\ T[k] = T[j] /\ ~Busy(k)
                       /\ intent[k] = NONE)
                      => Prefix(dlog[k], ilog[j])
Safety == AckedPreserved /\ EntryUnique /\ LogMatching /\ StateMatchesLog /\ PositionedPrefix /\ HistoryUnique
=============================================================================
