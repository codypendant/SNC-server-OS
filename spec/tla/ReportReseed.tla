---------------------------- MODULE ReportReseed ----------------------------
(* Report identity (Y9-T), the reseed grant wait and W-2, on ReconBlockers.  *)
(* Plan, lanes and expected results: docs/AFR_001_REPORT_MODEL_PREREG.md.    *)
EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS Nodes, P0, MaxTerm, MaxIds, MaxCrashes, MaxReps, MaxMsgs, MaxReseeds, MaxWipes,
          FixInc, FixTerm, FixWait, FixEntry, FixSendP, FixSendR, FixNonce, FixGrow

NONE == <<-1, -1>>
NOINT == [L |-> -1, t |-> 0]

VARIABLES rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep, lease,
          rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes, r7T,
          viol, seen, hist

vars == <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep, lease,
          rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes, r7T,
          viol, seen, hist>>

Min(S) == CHOOSE x \in S : \A y \in S : x <= y
Max(S) == CHOOSE x \in S : \A y \in S : x >= y
Prefix(s, u) == Len(s) <= Len(u) /\ SubSeq(u, 1, Len(s)) = s
LastT(l) == IF Len(l) = 0 THEN 0 ELSE l[Len(l)].t
Terms(l) == {l[i].t : i \in 1..Len(l)} \cup {0}
EndLE(l, t) == Max({i \in 0..Len(l) : i = 0 \/ l[i].t <= t})
NoRec == [m \in Nodes |-> NONE]
NoCov == [m \in Nodes |-> 0]

Init ==
  /\ rec = [term |-> 1, prim |-> P0, S |-> Nodes, ready |-> TRUE]
  /\ T = [n \in Nodes |-> 1]
  /\ isPrim = [n \in Nodes |-> n = P0]
  /\ dlog = [n \in Nodes |-> <<>>]
  /\ ilog = [n \in Nodes |-> <<>>]
  /\ A = [n \in Nodes |-> IF n = P0 THEN Nodes ELSE {}]
  /\ pos = [n \in Nodes |-> 1]
  /\ intent = [n \in Nodes |-> NOINT]
  /\ acked = {}
  /\ nid = 0
  /\ crashes = 0
  /\ inh = [n \in Nodes |-> 0]
  /\ pack = [n \in Nodes |-> 0]
  /\ rep = [n \in Nodes |-> {}]
  /\ lease = [n \in Nodes |-> 0]
  /\ rs = [n \in Nodes |-> FALSE]
  /\ ph = [n \in Nodes |-> 0]
  /\ src = [n \in Nodes |-> P0]
  /\ stopG = [n \in Nodes |-> FALSE]
  /\ booted = [n \in Nodes |-> TRUE]
  /\ incn = [n \in Nodes |-> <<0, 1>>]
  /\ nonce = 0
  /\ gr = [n \in Nodes |-> {}]
  /\ msgs = {}
  /\ pinc = [p \in Nodes |-> IF p = P0 THEN [m \in Nodes |-> <<0, 1>>] ELSE NoRec]
  /\ cov = [p \in Nodes |-> NoCov]
  /\ sv = [p \in Nodes |-> IF p = P0 THEN Nodes ELSE {}]
  /\ reseeds = 0
  /\ wipes = 0
  /\ r7T = 0
  /\ viol = {}
  /\ seen = {}
  /\ hist = {}

Pending(n) == intent[n] # NOINT
Current(p) == isPrim[p] /\ rec.prim = p /\ rec.term = T[p]
Serving(p) == isPrim[p] /\ ~Pending(p)
Barrier(p) == \A n \in Nodes : lease[n] = 0 \/ lease[n] >= T[p]
Follows(m, p) == pos[m] = T[p] /\ T[m] = T[p] /\ ~rs[m] /\ ~Pending(m)
Known(p, m) == pinc[p][m] # NONE
CaughtUp(p, m) == Known(p, m) /\ cov[p][m] >= Max({inh[p], pack[p]})
Holds(n, i, e) == Len(dlog[n]) >= i /\ dlog[n][i] = e

(* --- primary side ----------------------------------------------------- *)
NewEntry(p) ==
  /\ Serving(p) /\ nid < MaxIds
  /\ ilog' = [ilog EXCEPT ![p] = Append(@, [t |-> T[p], id |-> nid])]
  /\ nid' = nid + 1
  /\ UNCHANGED <<rec, T, isPrim, dlog, A, pos, intent, acked, crashes, inh, pack, rep, lease,
                 rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

WriteLocal(p) ==
  /\ isPrim[p] /\ Len(dlog[p]) < Len(ilog[p])
  /\ dlog' = [dlog EXCEPT ![p] = Append(@, ilog[p][Len(dlog[p]) + 1])]
  /\ UNCHANGED <<rec, T, isPrim, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep, lease,
                 rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

Ship(p, m) ==
  /\ Serving(p) /\ m # p /\ Follows(m, p)
  /\ Len(dlog[m]) < Len(ilog[p])
  /\ dlog' = [dlog EXCEPT ![m] = Append(@, ilog[p][Len(dlog[m]) + 1])]
  /\ UNCHANGED <<rec, T, isPrim, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep, lease,
                 rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

Ack(p, i) ==
  /\ Serving(p) /\ Barrier(p) /\ i \in 1..Len(dlog[p])
  /\ Cardinality(sv[p]) >= 2 /\ sv[p] \subseteq A[p]
  /\ \A m \in A[p] \ {p} : Known(p, m) /\ cov[p][m] >= i /\ p \in gr[m]
  /\ LET e == dlog[p][i]
         sound == /\ \A m \in A[p] : Holds(m, i, e) /\ ~rs[m]
                  /\ Cardinality({n \in Nodes : Holds(n, i, e)}) >= 2
     IN /\ acked' = acked \cup {<<i, e, T[p]>>}
        /\ viol' = IF sound THEN viol ELSE viol \cup {"ack"}
  /\ pack' = [pack EXCEPT ![p] = Max({@, i})]
  /\ seen' = IF r7T > 0 /\ T[p] >= r7T THEN seen \cup {"ackr7"} ELSE seen
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, nid, crashes, inh, rep, lease,
                 rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T>>

AddA(p, c) ==
  /\ Current(p) /\ Serving(p) /\ c # p /\ c \notin A[p] /\ Known(p, c)
  /\ A' = [A EXCEPT ![p] = @ \cup {c}]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, pos, intent, acked, nid, crashes, inh, pack, rep, lease,
                 rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

Grow(p, c) ==
  /\ Current(p) /\ Serving(p) /\ c \in A[p] /\ c \notin rec.S /\ CaughtUp(p, c)
  /\ FixGrow => p \in gr[c]
  /\ rec' = [rec EXCEPT !.S = @ \cup {c}]
  /\ sv' = [sv EXCEPT ![p] = @ \cup {c}]
  /\ LET k == Max({inh[p], pack[p]}) IN
       viol' = IF Len(dlog[c]) >= k /\ SubSeq(dlog[c], 1, k) = SubSeq(ilog[p], 1, k)
               THEN viol ELSE viol \cup {"grow"}
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep, lease,
                 rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, reseeds, wipes,
                 r7T, seen>>

Shrink(p, c) ==
  /\ Current(p) /\ c \in rec.S /\ c # p
  /\ rec' = [rec EXCEPT !.S = @ \ {c}]
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep, lease,
                 rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

LearnS(p) ==
  /\ isPrim[p] /\ rec.term = T[p] /\ sv[p] # rec.S
  /\ sv' = [sv EXCEPT ![p] = rec.S]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep,
                 lease, rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, reseeds, wipes,
                 r7T, viol, seen>>

Drop(p, c) ==
  /\ isPrim[p] /\ c \in A[p] /\ c # p /\ c \notin sv[p] /\ rec.term = T[p]
  /\ lease[c] # T[p] /\ p \notin gr[c]
  /\ A' = [A EXCEPT ![p] = @ \ {c}]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, pos, intent, acked, nid, crashes, inh, pack, rep, lease,
                 rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

MarkReady(p) ==
  /\ Current(p) /\ Serving(p) /\ Barrier(p) /\ ~rec.ready
  /\ \A m \in rec.S \ {p} : Known(p, m)
  /\ rec' = [rec EXCEPT !.ready = TRUE]
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep, lease,
                 rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

SGrant(p, m) ==
  /\ Current(p) /\ Serving(p) /\ Barrier(p) /\ m # p /\ m \in A[p] /\ lease[m] = 0
  /\ CaughtUp(p, m)
  /\ lease' = [lease EXCEPT ![m] = T[p]]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep,
                 rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

LExpire(m) ==
  /\ lease[m] # 0
  /\ lease' = [lease EXCEPT ![m] = 0]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep,
                 rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

(* --- authority grants, untimed (R3, R4, R7) --------------------------- *)
Grant(k, p) ==
  /\ k # p /\ isPrim[p] /\ T[k] = T[p] /\ ~rs[k] /\ ~stopG[k] /\ booted[k] /\ p \notin gr[k]
  /\ gr' = [gr EXCEPT ![k] = @ \cup {p}]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep,
                 lease, rs, ph, src, stopG, booted, incn, nonce, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

Expire(k, p) ==
  /\ p \in gr[k]
  /\ gr' = [gr EXCEPT ![k] = @ \ {p}]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep,
                 lease, rs, ph, src, stopG, booted, incn, nonce, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

BootRead(n) ==
  /\ ~booted[n] /\ T[n] = rec.term
  /\ booted' = [booted EXCEPT ![n] = TRUE]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep,
                 lease, rs, ph, src, stopG, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

(* --- reports (Y9-T) --------------------------------------------------- *)
Send(m) ==
  /\ Len(dlog[m]) > 0 /\ Cardinality(msgs) < MaxMsgs
  /\ FixSendP => ~Pending(m)
  /\ FixSendR => ~rs[m]
  /\ LET r == [from |-> m, t |-> LastT(dlog[m]), l |-> Len(dlog[m]), inc |-> incn[m], term |-> T[m]]
     IN r \notin msgs /\ msgs' = msgs \cup {r}
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep,
                 lease, rs, ph, src, stopG, booted, incn, nonce, gr, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

DropMsg(r) ==
  /\ r \in msgs
  /\ msgs' = msgs \ {r}
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep,
                 lease, rs, ph, src, stopG, booted, incn, nonce, gr, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

Deliver(p, r) ==
  /\ r \in msgs /\ isPrim[p] /\ r.from # p /\ Known(p, r.from)
  /\ LET okE == r.l <= Len(ilog[p]) /\ ilog[p][r.l].t = r.t
         okI == r.inc = pinc[p][r.from]
         okT == r.term = T[p]
     IN IF (FixEntry => okE) /\ (FixInc => okI) /\ (FixTerm => okT)
        THEN /\ cov[p][r.from] < r.l
             /\ cov' = [cov EXCEPT ![p][r.from] = r.l]
             /\ UNCHANGED seen
        ELSE /\ seen' = seen \cup (IF ~okI /\ okT /\ okE THEN {"inc"}
                                   ELSE IF okI /\ ~okT /\ okE THEN {"term"}
                                   ELSE IF okI /\ okT /\ ~okE THEN {"entry"} ELSE {})
             /\ UNCHANGED cov
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep,
                 lease, rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, sv, reseeds, wipes,
                 r7T, viol>>

(* --- promotion -------------------------------------------------------- *)
Promote(j) ==
  /\ rec.term < MaxTerm /\ j \in rec.S /\ ~rs[j] /\ ~stopG[j]
  /\ \/ j # rec.prim
     \/ rec.S = {j} /\ ~isPrim[j] /\ T[j] = rec.term
  /\ rec' = [term |-> rec.term + 1, prim |-> j, ready |-> FALSE,
             S |-> IF j = rec.prim THEN {j} ELSE rec.S \ {rec.prim}]
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep, lease,
                 rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

OpOutside(j) ==
  /\ rec.term < MaxTerm /\ rec.ready /\ Cardinality(rec.S) = 1 /\ j \notin rec.S /\ ~rs[j] /\ ~stopG[j]
  /\ rec' = [term |-> rec.term + 1, prim |-> j, ready |-> FALSE, S |-> {j}]
  /\ r7T' = rec.term + 1
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep, lease,
                 rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes,
                 viol, seen>>

Learn(n) ==
  /\ rec.term > T[n]
  /\ T' = [T EXCEPT ![n] = rec.term]
  /\ isPrim' = [isPrim EXCEPT ![n] = (rec.prim = n)]
  /\ A' = [A EXCEPT ![n] = IF rec.prim = n THEN rec.S ELSE {}]
  /\ sv' = [sv EXCEPT ![n] = IF rec.prim = n THEN rec.S ELSE {}]
  /\ ilog' = [ilog EXCEPT ![n] = dlog[n]]
  /\ pos' = [pos EXCEPT ![n] = IF rec.prim = n THEN rec.term ELSE 0]
  /\ inh' = [inh EXCEPT ![n] = IF rec.prim = n THEN Len(dlog[n]) ELSE 0]
  /\ pack' = [pack EXCEPT ![n] = 0]
  /\ lease' = [lease EXCEPT ![n] = 0]
  /\ pinc' = [pinc EXCEPT ![n] = IF rec.prim = n THEN [m \in Nodes |-> IF m = n THEN incn[n] ELSE NONE]
                                 ELSE NoRec]
  /\ cov' = [cov EXCEPT ![n] = NoCov]
  /\ UNCHANGED <<rec, dlog, intent, acked, nid, crashes, rep, rs, ph, src, stopG, booted, incn, nonce,
                 gr, msgs, reseeds, wipes, r7T, viol, seen>>

(* --- reconciliation (R6.2-R6.4) --------------------------------------- *)
Answer(k, j) ==
  LET ts == Max({x \in Terms(ilog[j]) : x <= LastT(dlog[k])})
  IN  [ts |-> ts, L |-> Min({EndLE(ilog[j], ts), EndLE(dlog[k], ts)})]

ReconStep(k, j) ==
  /\ Serving(j) /\ k # j /\ T[k] = T[j] /\ pos[k] # T[j] /\ ~rs[k] /\ ~Pending(k)
  /\ Cardinality(rep[k]) < MaxReps
  /\ LET a == Answer(k, j)
         done == a.L = Len(dlog[k]) /\ a.ts = LastT(dlog[k])
     IN /\ rep' = [rep EXCEPT ![k] = @ \cup {[tag |-> T[j], L |-> a.L, ts |-> a.ts,
                                              blt |-> LastT(dlog[k]), ble |-> Len(dlog[k])]}]
        /\ IF done \/ incn[k] # pinc[j][k]
           THEN /\ pinc' = [pinc EXCEPT ![j][k] = IF done THEN incn[k] ELSE NONE]
                /\ cov' = [cov EXCEPT ![j][k] = 0]
           ELSE UNCHANGED <<pinc, cov>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease,
                 rs, ph, src, stopG, booted, incn, nonce, gr, msgs, sv, reseeds, wipes, r7T, viol,
                 seen>>

Usable(k, r) == /\ r.tag = T[k] /\ ~isPrim[k] /\ pos[k] # T[k] /\ ~rs[k]
                /\ r.blt = LastT(dlog[k]) /\ r.ble = Len(dlog[k])

RecvRep(k, r) ==
  /\ r \in rep[k] /\ Usable(k, r) /\ ~Pending(k)
  /\ IF r.L = Len(dlog[k]) /\ r.ts = LastT(dlog[k])
     THEN /\ pos' = [pos EXCEPT ![k] = T[k]]
          /\ UNCHANGED intent
     ELSE /\ intent' = [intent EXCEPT ![k] = [L |-> r.L, t |-> T[k]]]
          /\ UNCHANGED pos
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, acked, nid, crashes, inh, pack, rep, lease,
                 rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

DropRep(k, r) ==
  /\ r \in rep[k]
  /\ rep' = [rep EXCEPT ![k] = @ \ {r}]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease,
                 rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

ApplyIntent(k) ==
  /\ Pending(k) /\ Len(dlog[k]) > intent[k].L
  /\ dlog' = [dlog EXCEPT ![k] = SubSeq(@, 1, intent[k].L)]
  /\ ilog' = [ilog EXCEPT ![k] = SubSeq(@, 1, Min({intent[k].L, Len(@)}))]
  /\ UNCHANGED <<rec, T, isPrim, A, pos, intent, acked, nid, crashes, inh, pack, rep, lease,
                 rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

ClearIntent(k) ==
  /\ Pending(k) /\ Len(dlog[k]) <= intent[k].L
  /\ intent' = [intent EXCEPT ![k] = NOINT]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, acked, nid, crashes, inh, pack, rep, lease,
                 rs, ph, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

(* --- reseed in place (contract 6.5, R6.5 revision) -------------------- *)
RS0(k) ==
  /\ reseeds < MaxReseeds /\ k \notin rec.S /\ ~isPrim[k] /\ ~rs[k] /\ ~stopG[k] /\ ~Pending(k)
  /\ reseeds' = reseeds + 1
  /\ stopG' = [stopG EXCEPT ![k] = TRUE]
  /\ lease' = [lease EXCEPT ![k] = 0]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep,
                 rs, ph, src, booted, incn, nonce, gr, msgs, pinc, cov, sv, wipes, r7T, viol, seen>>

RS1(k) ==
  /\ stopG[k] /\ ~rs[k] /\ ~isPrim[k] /\ ~Pending(k) /\ k \notin rec.S
  /\ FixWait => gr[k] = {}
  /\ rs' = [rs EXCEPT ![k] = TRUE]
  /\ ph' = [ph EXCEPT ![k] = 1]
  /\ incn' = [incn EXCEPT ![k] = <<@[1], @[2] + 1>>]
  /\ pos' = [pos EXCEPT ![k] = 0]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, intent, acked, nid, crashes, inh, pack, rep, lease,
                 src, stopG, booted, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes, r7T, viol, seen>>

RS2(k) ==
  /\ rs[k] /\ ph[k] = 1
  /\ dlog' = [dlog EXCEPT ![k] = <<>>]
  /\ ilog' = [ilog EXCEPT ![k] = <<>>]
  /\ ph' = [ph EXCEPT ![k] = 2]
  /\ UNCHANGED <<rec, T, isPrim, A, pos, intent, acked, nid, crashes, inh, pack, rep, lease,
                 rs, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes, r7T,
                 viol, seen>>

RS3(k, j) ==
  /\ rs[k] /\ ph[k] = 2 /\ j # k /\ Serving(j) /\ T[j] >= T[k]
  /\ T' = [T EXCEPT ![k] = T[j]]
  /\ src' = [src EXCEPT ![k] = j]
  /\ pinc' = [pinc EXCEPT ![j][k] = NONE]
  /\ cov' = [cov EXCEPT ![j][k] = 0]
  /\ ph' = [ph EXCEPT ![k] = 3]
  /\ UNCHANGED <<rec, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep, lease,
                 rs, stopG, booted, incn, nonce, gr, msgs, sv, reseeds, wipes, r7T, viol, seen>>

SrcOK(k) == Serving(src[k]) /\ T[src[k]] = T[k]

RS5(k, n) ==
  /\ rs[k] /\ ph[k] = 3 /\ SrcOK(k) /\ n \in 0..Len(dlog[src[k]])
  /\ dlog' = [dlog EXCEPT ![k] = SubSeq(dlog[src[k]], 1, n)]
  /\ ilog' = [ilog EXCEPT ![k] = SubSeq(dlog[src[k]], 1, n)]
  /\ ph' = [ph EXCEPT ![k] = 4]
  /\ UNCHANGED <<rec, T, isPrim, A, pos, intent, acked, nid, crashes, inh, pack, rep, lease,
                 rs, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes, r7T,
                 viol, seen>>

RS6(k) ==
  /\ rs[k] /\ ph[k] = 4 /\ SrcOK(k)
  /\ rs' = [rs EXCEPT ![k] = FALSE]
  /\ ph' = [ph EXCEPT ![k] = 0]
  /\ stopG' = [stopG EXCEPT ![k] = FALSE]
  /\ pos' = [pos EXCEPT ![k] = 0]
  /\ seen' = IF \E r \in msgs : r.from = k /\ r.l > Len(dlog[k]) THEN seen \cup {"short"} ELSE seen
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, intent, acked, nid, crashes, inh, pack, rep, lease,
                 src, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes, r7T, viol>>

RSBack(k) ==
  /\ rs[k] /\ ph[k] >= 3 /\ ~SrcOK(k)
  /\ ph' = [ph EXCEPT ![k] = 1]
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rep,
                 lease, rs, src, stopG, booted, incn, nonce, gr, msgs, pinc, cov, sv, reseeds, wipes,
                 r7T, viol, seen>>

Wipe(k, j, n) ==
  /\ wipes < MaxWipes /\ k \notin rec.S /\ ~isPrim[k] /\ gr[k] = {}
  /\ j # k /\ Serving(j) /\ n \in 0..Len(dlog[j])
  /\ wipes' = wipes + 1
  /\ dlog' = [dlog EXCEPT ![k] = SubSeq(dlog[j], 1, n)]
  /\ ilog' = [ilog EXCEPT ![k] = SubSeq(dlog[j], 1, n)]
  /\ T' = [T EXCEPT ![k] = T[j]]
  /\ nonce' = IF FixNonce THEN nonce + 1 ELSE nonce
  /\ incn' = [incn EXCEPT ![k] = <<IF FixNonce THEN nonce + 1 ELSE @[1], 1>>]
  /\ pos' = [pos EXCEPT ![k] = 0]
  /\ intent' = [intent EXCEPT ![k] = NOINT]
  /\ lease' = [lease EXCEPT ![k] = 0]
  /\ stopG' = [stopG EXCEPT ![k] = FALSE]
  /\ booted' = [booted EXCEPT ![k] = FALSE]
  /\ rep' = [rep EXCEPT ![k] = {}]
  /\ pinc' = [pinc EXCEPT ![j][k] = NONE]
  /\ cov' = [cov EXCEPT ![j][k] = 0]
  /\ rs' = [rs EXCEPT ![k] = FALSE]
  /\ ph' = [ph EXCEPT ![k] = 0]
  /\ seen' = seen \cup {"wipe"}
  /\ UNCHANGED <<rec, isPrim, A, acked, nid, crashes, inh, pack, src, gr, msgs, sv, reseeds, r7T,
                 viol>>

(* --- crash and recovery ----------------------------------------------- *)
Crash(n) ==
  /\ crashes < MaxCrashes
  /\ crashes' = crashes + 1
  /\ dlog' = [dlog EXCEPT ![n] = IF Pending(n) THEN SubSeq(@, 1, Min({intent[n].L, Len(@)})) ELSE @]
  /\ ilog' = [ilog EXCEPT ![n] = dlog'[n]]
  /\ isPrim' = [isPrim EXCEPT ![n] = FALSE]
  /\ A' = [A EXCEPT ![n] = {}]
  /\ sv' = [sv EXCEPT ![n] = {}]
  /\ pos' = [pos EXCEPT ![n] = 0]
  /\ intent' = [intent EXCEPT ![n] = NOINT]
  /\ lease' = [lease EXCEPT ![n] = 0]
  /\ pinc' = [pinc EXCEPT ![n] = NoRec]
  /\ cov' = [cov EXCEPT ![n] = NoCov]
  /\ booted' = [booted EXCEPT ![n] = FALSE]
  /\ ph' = [ph EXCEPT ![n] = IF rs[n] THEN 1 ELSE 0]
  /\ incn' = [incn EXCEPT ![n] = <<@[1], @[2] + 1>>]
  /\ seen' = IF rs[n] THEN seen \cup {"crashrs"} ELSE seen
  /\ UNCHANGED <<rec, T, acked, nid, inh, pack, rep, rs, src, stopG, nonce, gr, msgs, reseeds, wipes,
                 r7T, viol>>

Step ==
  \/ \E p \in Nodes : NewEntry(p) \/ WriteLocal(p) \/ Promote(p) \/ OpOutside(p) \/ Learn(p)
                      \/ MarkReady(p) \/ ApplyIntent(p) \/ ClearIntent(p) \/ Crash(p) \/ LExpire(p)
                      \/ LearnS(p) \/ BootRead(p) \/ Send(p) \/ RS0(p) \/ RS1(p) \/ RS2(p)
                      \/ RS6(p) \/ RSBack(p)
  \/ \E p, m \in Nodes : Ship(p, m) \/ AddA(p, m) \/ Grow(p, m) \/ Shrink(p, m) \/ Drop(p, m)
                         \/ ReconStep(m, p) \/ SGrant(p, m) \/ Grant(m, p) \/ Expire(m, p)
                         \/ RS3(m, p)
  \/ \E p \in Nodes, i \in 1..MaxIds : Ack(p, i)
  \/ \E k \in Nodes, n \in 0..MaxIds : RS5(k, n)
  \/ \E k, j \in Nodes, n \in 0..MaxIds : Wipe(k, j, n)
  \/ \E k \in Nodes : \E r \in rep[k] : RecvRep(k, r) \/ DropRep(k, r)
  \/ \E r \in msgs : DropMsg(r) \/ \E p \in Nodes : Deliver(p, r)

Held(l) == UNION {{<<i, l[n][i]>> : i \in 1..Len(l[n])} : n \in Nodes}
Next == Step /\ hist' = hist \cup Held(dlog') \cup Held(ilog')

Spec == Init /\ [][Next]_vars
Sym == Permutations(Nodes \ {P0})

(* --- properties ------------------------------------------------------- *)
AckSound == "ack" \notin viol
GrowSound == "grow" \notin viol
AckedPreserved == \A e \in acked : e[3] >= r7T => \A n \in rec.S : Holds(n, e[1], e[2])
EntryUnique == \A a, b \in Nodes : \A i \in 1..Min({Len(dlog[a]), Len(dlog[b])}) :
                 dlog[a][i].t = dlog[b][i].t => dlog[a][i] = dlog[b][i]
HistoryUnique == \A x, y \in hist : (x[1] = y[1] /\ x[2].t = y[2].t) => x[2] = y[2]
LogMatching == \A a, b \in Nodes : \A i \in 1..Min({Len(dlog[a]), Len(dlog[b])}) :
                 dlog[a][i] = dlog[b][i] => SubSeq(dlog[a], 1, i) = SubSeq(dlog[b], 1, i)
Safety == AckSound /\ GrowSound /\ AckedPreserved /\ EntryUnique /\ LogMatching /\ HistoryUnique

(* --- reachability probes: TLC must report each one violated ---------- *)
NoRejectByInc == "inc" \notin seen
NoRejectByTerm == "term" \notin seen
NoRejectByEntry == "entry" \notin seen
NoShortReseed == "short" \notin seen
NoWipe == "wipe" \notin seen
NoCrashInReseed == "crashrs" \notin seen
NoAckAfterR7 == "ackr7" \notin seen
=============================================================================
