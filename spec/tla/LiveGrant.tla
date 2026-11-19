---------------------------- MODULE LiveGrant ----------------------------
(* LG-1..LG-6 and the R1.1 change on ReportReseed.tla.                    *)
(* Plan, lanes and expected results: docs/AFR_001_LIVE_GRANT_PREREG.md.   *)
EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS Nodes, P0, MaxTerm, MaxIds, MaxCrashes, MaxReps, MaxMsgs, MaxGot, MaxReqs, MaxReseeds,
          MaxWipes, MaxVer, Repl, OpInside,
          FixGrantInc, FixGenI, FixWaitT, FixWaitR, FixWaitW, FixLG5, FixEntry, FixSendP, FixSendR,
          FixNonce, FixCovGrow, AddTerm, FixStop, FixGenR, FixOrder, FixGenB, FixCont,
          FixRead, FixAdmGrant, FixRefuse, FixFreshDur, RecAct

NONE == <<-1, -1>>
NOINT == [L |-> -1, t |-> 0]
NOWANT == [L |-> -1, t |-> 0, blt |-> -1, ble |-> -1]

VARIABLES rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease,
          rs, ph, src, rst, booted, incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want,
          stopG, lg, reseeds, wipes, r7T, viol, seen, hist, act

vars == <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease,
          rs, ph, src, rst, booted, incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want,
          stopG, lg, reseeds, wipes, r7T, viol, seen, hist, act>>

Min(S) == CHOOSE x \in S : \A y \in S : x <= y
Max(S) == CHOOSE x \in S : \A y \in S : x >= y
LastT(l) == IF Len(l) = 0 THEN 0 ELSE l[Len(l)].t
Terms(l) == {l[i].t : i \in 1..Len(l)} \cup {0}
EndLE(l, t) == Max({i \in 0..Len(l) : i = 0 \/ l[i].t <= t})
NoRec == [m \in Nodes |-> NONE]
Down == {4, 5}

Init ==
  /\ rec = [ver |-> 0, term |-> 1, prim |-> P0, S |-> Nodes, ready |-> TRUE]
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
  /\ lease = [n \in Nodes |-> 0]
  /\ rs = [n \in Nodes |-> FALSE]
  /\ ph = [n \in Nodes |-> 0]
  /\ src = [n \in Nodes |-> P0]
  /\ rst = [n \in Nodes |-> 0]
  /\ booted = [n \in Nodes |-> TRUE]
  /\ incn = [n \in Nodes |-> <<0, 1>>]
  /\ nonce = 0
  /\ gr = [n \in Nodes |-> {}]
  /\ msgs = {}
  /\ got = [n \in Nodes |-> {}]
  /\ pinc = [p \in Nodes |-> IF p = P0 THEN [m \in Nodes |-> <<0, 1>>] ELSE NoRec]
  /\ sv = [p \in Nodes |-> IF p = P0 THEN Nodes ELSE {}]
  /\ gp = {}
  /\ req = {}
  /\ rep = [n \in Nodes |-> {}]
  /\ want = [n \in Nodes |-> NOWANT]
  /\ stopG = [n \in Nodes |-> FALSE]
  /\ lg = [n \in Nodes |-> 0]
  /\ reseeds = 0
  /\ wipes = 0
  /\ r7T = 0
  /\ viol = {}
  /\ seen = {}
  /\ hist = {}
  /\ act = <<>>

Pending(n) == intent[n] # NOINT
Current(p) == isPrim[p] /\ rec.prim = p /\ rec.term = T[p]
Serving(p) == isPrim[p] /\ ~Pending(p)
Barrier(p) == \A n \in Nodes : lease[n] = 0 \/ lease[n] >= T[p]
Known(p, m) == pinc[p][m] # NONE
Holds(n, i, e) == Len(dlog[n]) >= i /\ dlog[n][i] = e
HoldsPre(n, s) == Len(s) = 0 \/ (Len(dlog[n]) >= Len(s) /\ SubSeq(dlog[n], 1, Len(s)) = s /\ ~rs[n])
Waited(k) == gr[k] = {}
Need(p) == Max({inh[p], pack[p]})

OkE(p, r) == FixEntry => (r.l <= Len(ilog[p]) /\ ilog[p][r.l].t = r.t)
Live(p, m, r) == IF FixGrantInc THEN <<p, r.inc>> \in gr[m]
                 ELSE (\E x \in gr[m] : x[1] = p) /\ r.inc = pinc[p][m]
Cred(p, m) == {r \in got[p] : r.from = m /\ OkE(p, r) /\ Live(p, m, r) /\ (AddTerm => r.term = T[p])}
Cov(p, m) == Max({r.l : r \in Cred(p, m)} \cup {0})
CovG(p, m) == IF FixCovGrow THEN Cov(p, m)
              ELSE Max({r.l : r \in {x \in got[p] : x.from = m /\ OkE(p, x)}} \cup {0})
CaughtUp(p, m, c) == Known(p, m) /\ c >= Need(p)

Chg(r) == rec.ver < MaxVer /\ rec' = [r EXCEPT !.ver = rec.ver + 1] /\ gp' = {}

(* --- primary side ----------------------------------------------------- *)
NewEntry(p) ==
  /\ Serving(p) /\ nid < MaxIds
  /\ ilog' = [ilog EXCEPT ![p] = Append(@, [t |-> T[p], id |-> nid])]
  /\ nid' = nid + 1
  /\ act' = IF RecAct THEN <<"NewEntry", p>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, A, pos, intent, acked, crashes, inh, pack, lease, rs, ph, src, rst,
                 booted, incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds, wipes,
                 r7T, viol, seen>>

WriteLocal(p) ==
  /\ isPrim[p] /\ Len(dlog[p]) < Len(ilog[p])
  /\ dlog' = [dlog EXCEPT ![p] = Append(@, ilog[p][Len(dlog[p]) + 1])]
  /\ act' = IF RecAct THEN <<"WriteLocal", p>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph, src,
                 rst, booted, incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

Ship(p, m, i) ==
  /\ Serving(p) /\ m # p /\ T[m] = T[p] /\ pos[m] = T[p] /\ ~rs[m] /\ ~Pending(m)
  /\ i \in 1..Len(ilog[p])
  /\ IF FixCont THEN Len(dlog[m]) = i - 1 /\ (IF i = 1 THEN TRUE ELSE dlog[m][i - 1].t = ilog[p][i - 1].t)
     ELSE i > Len(dlog[m])
  /\ dlog' = [dlog EXCEPT ![m] = Append(@, ilog[p][i])]
  /\ act' = IF RecAct THEN <<"Ship", p, m, i>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph, src,
                 rst, booted, incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

Ack(p, i) ==
  /\ Serving(p) /\ Barrier(p) /\ i \in 1..Len(dlog[p])
  /\ Cardinality(sv[p]) >= 2 /\ sv[p] \subseteq A[p]
  /\ \A m \in A[p] \ {p} : Known(p, m) /\ Cov(p, m) >= i
  /\ LET e == dlog[p][i]
         Keeps(n) == Holds(n, i, e) /\ ~(Pending(n) /\ intent[n].L < i)
         sound == /\ \A m \in A[p] : Keeps(m) /\ ~rs[m]
                  /\ Cardinality({n \in Nodes : Keeps(n)}) >= 2
     IN /\ acked' = acked \cup {<<i, e, T[p]>>}
        /\ viol' = IF sound THEN viol ELSE viol \cup {"ack"}
  /\ pack' = [pack EXCEPT ![p] = Max({@, i})]
  /\ seen' = IF r7T > 0 /\ T[p] >= r7T THEN seen \cup {"ackr7"} ELSE seen
  /\ act' = IF RecAct THEN <<"Ack", p, i>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, nid, crashes, inh, lease, rs, ph, src, rst,
                 booted, incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds, wipes,
                 r7T>>

AddA(p, c) ==
  /\ Current(p) /\ Serving(p) /\ c # p /\ c \notin A[p] /\ Known(p, c)
  /\ A' = [A EXCEPT ![p] = @ \cup {c}]
  /\ act' = IF RecAct THEN <<"AddA", p, c>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph, src,
                 rst, booted, incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

ProposeGrow(p, c) ==
  /\ Current(p) /\ Serving(p) /\ c \in A[p] /\ c \notin rec.S /\ gp = {}
  /\ CaughtUp(p, c, CovG(p, c)) /\ (FixAdmGrant => <<p, pinc[p][c]>> \in gr[c])
  /\ gp' = {[c |-> c, ver |-> rec.ver, pre |-> SubSeq(ilog[p], 1, Need(p))]}
  /\ sv' = [sv EXCEPT ![p] = @ \cup {c}]
  /\ act' = IF RecAct THEN <<"ProposeGrow", p, c>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph,
                 src, rst, booted, incn, nonce, gr, msgs, got, pinc, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

ChooseGrow(x) ==
  /\ x \in gp /\ rec.ver = x.ver /\ x.c \notin rec.S
  /\ Chg([rec EXCEPT !.S = @ \cup {x.c}])
  /\ viol' = IF HoldsPre(x.c, x.pre) THEN viol ELSE viol \cup {"grow"}
  /\ act' = IF RecAct THEN <<"ChooseGrow", x>> ELSE <<>>
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph, src,
                 rst, booted, incn, nonce, gr, msgs, got, pinc, sv, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, seen>>

Shrink(p, c) ==
  /\ Current(p) /\ c \in rec.S /\ c # p
  /\ Chg([rec EXCEPT !.S = @ \ {c}])
  /\ act' = IF RecAct THEN <<"Shrink", p, c>> ELSE <<>>
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph, src,
                 rst, booted, incn, nonce, gr, msgs, got, pinc, sv, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

LearnS(p) ==
  LET Undecided == rec.S \cup (IF FixRead THEN {x.c : x \in {y \in gp : y.ver = rec.ver}} ELSE {}) IN
  /\ isPrim[p] /\ rec.term = T[p] /\ sv[p] # Undecided
  /\ sv' = [sv EXCEPT ![p] = Undecided]
  /\ act' = IF RecAct THEN <<"LearnS", p>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph,
                 src, rst, booted, incn, nonce, gr, msgs, got, pinc, gp, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

Drop(p, c) ==
  /\ isPrim[p] /\ c \in A[p] /\ c # p /\ c \notin sv[p] /\ rec.term = T[p] /\ lease[c] # T[p]
  /\ A' = [A EXCEPT ![p] = @ \ {c}]
  /\ act' = IF RecAct THEN <<"Drop", p, c>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph, src,
                 rst, booted, incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

MarkReady(p) ==
  /\ Current(p) /\ Serving(p) /\ Barrier(p) /\ ~rec.ready
  /\ \A m \in rec.S \ {p} : Known(p, m)
  /\ Chg([rec EXCEPT !.ready = TRUE])
  /\ act' = IF RecAct THEN <<"MarkReady", p>> ELSE <<>>
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph, src,
                 rst, booted, incn, nonce, gr, msgs, got, pinc, sv, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

SGrant(p, m) ==
  /\ Serving(p) /\ Barrier(p) /\ m # p /\ m \in A[p] /\ lease[m] = 0
  /\ T[m] = T[p] /\ ~Pending(m) /\ ~rs[m] /\ lg[m] = 0 /\ booted[m]
  /\ CaughtUp(p, m, Cov(p, m))
  /\ lease' = [lease EXCEPT ![m] = T[p]]
  /\ viol' = IF HoldsPre(m, SubSeq(ilog[p], 1, Need(p))) THEN viol ELSE viol \cup {"lease"}
  /\ act' = IF RecAct THEN <<"SGrant", p, m>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rs, ph, src,
                 rst, booted, incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, seen>>

LExpire(m) ==
  /\ lease[m] # 0
  /\ lease' = [lease EXCEPT ![m] = 0]
  /\ act' = IF RecAct THEN <<"LExpire", m>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rs, ph, src,
                 rst, booted, incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

(* --- authority grants, untimed, naming the incarnation (LG-2) --------- *)
Grant(k, p) ==
  /\ k # p /\ isPrim[p] /\ T[k] = T[p] /\ pos[k] = T[k] /\ ~Pending(k) /\ ~rs[k] /\ booted[k]
  /\ lg[k] \notin Down \cup {6} /\ (FixStop => ~stopG[k]) /\ <<p, incn[k]>> \notin gr[k]
  /\ gr' = [gr EXCEPT ![k] = @ \cup {<<p, incn[k]>>}]
  /\ act' = IF RecAct THEN <<"Grant", k, p>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph,
                 src, rst, booted, incn, nonce, msgs, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

Expire(k, x) ==
  /\ x \in gr[k]
  /\ gr' = [gr EXCEPT ![k] = @ \ {x}]
  /\ act' = IF RecAct THEN <<"Expire", k, x>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph,
                 src, rst, booted, incn, nonce, msgs, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

BootRead(n) ==
  /\ ~booted[n] /\ T[n] = rec.term /\ lg[n] \notin Down
  /\ booted' = [booted EXCEPT ![n] = TRUE]
  /\ act' = IF RecAct THEN <<"BootRead", n>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph,
                 src, rst, incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

(* --- reports (R9-A', LG-2, LG-3) -------------------------------------- *)
Send(m) ==
  /\ Len(dlog[m]) > 0 /\ Cardinality(msgs) < MaxMsgs /\ booted[m] /\ lg[m] \notin Down
  /\ FixSendP => ~Pending(m)
  /\ FixSendR => ~rs[m]
  /\ LET r == [from |-> m, t |-> LastT(dlog[m]), l |-> Len(dlog[m]), inc |-> incn[m],
               term |-> IF AddTerm THEN T[m] ELSE 0]
     IN r \notin msgs /\ msgs' = msgs \cup {r}
  /\ act' = IF RecAct THEN <<"Send", m>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph,
                 src, rst, booted, incn, nonce, gr, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

DropMsg(r) ==
  /\ r \in msgs
  /\ msgs' = msgs \ {r}
  /\ act' = IF RecAct THEN <<"DropMsg", r>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph,
                 src, rst, booted, incn, nonce, gr, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

Deliver(p, r) ==
  /\ r \in msgs /\ isPrim[p] /\ r.from # p /\ r \notin got[p] /\ Cardinality(got[p]) < MaxGot
  /\ got' = [got EXCEPT ![p] = @ \cup {r}]
  /\ seen' = IF (\E x \in gr[r.from] : x[1] = p) /\ <<p, r.inc>> \notin gr[r.from] /\ OkE(p, r)
             THEN seen \cup {"inc"} ELSE seen
  /\ act' = IF RecAct THEN <<"Deliver", p, r>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph,
                 src, rst, booted, incn, nonce, gr, msgs, pinc, sv, gp, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol>>

(* --- promotion -------------------------------------------------------- *)
Promote(j) ==
  /\ rec.term < MaxTerm /\ j \in rec.S
  /\ rec.ready \/ OpInside
  /\ \/ j # rec.prim
     \/ rec.S = {j} /\ ~isPrim[j] /\ T[j] = rec.term
  /\ Chg([rec EXCEPT !.term = rec.term + 1, !.prim = j, !.ready = FALSE,
                     !.S = IF j = rec.prim THEN {j} ELSE rec.S \ {rec.prim}])
  /\ act' = IF RecAct THEN <<"Promote", j>> ELSE <<>>
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph, src,
                 rst, booted, incn, nonce, gr, msgs, got, pinc, sv, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

OpOutside(j) ==
  /\ rec.term < MaxTerm /\ rec.ready /\ Cardinality(rec.S) = 1 /\ j \notin rec.S
  /\ Chg([rec EXCEPT !.term = rec.term + 1, !.prim = j, !.ready = FALSE, !.S = {j}])
  /\ r7T' = rec.term + 1
  /\ act' = IF RecAct THEN <<"OpOutside", j>> ELSE <<>>
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph, src,
                 rst, booted, incn, nonce, gr, msgs, got, pinc, sv, req, rep, want, stopG, lg, reseeds,
                 wipes, viol, seen>>

Abandons(n) == want[n] # NOWANT \/ (lg[n] \in {1, 2} /\ ~rs[n])

Becomes(n) == rec.prim = n /\ (FixRefuse => lg[n] # 6 /\ ~rs[n] /\ booted[n])

Learn(n) ==
  /\ rec.term > T[n] /\ lg[n] \notin Down
  /\ T' = [T EXCEPT ![n] = rec.term]
  /\ isPrim' = [isPrim EXCEPT ![n] = Becomes(n)]
  /\ A' = [A EXCEPT ![n] = IF Becomes(n) THEN rec.S ELSE {}]
  /\ sv' = [sv EXCEPT ![n] = IF Becomes(n) THEN rec.S ELSE {}]
  /\ ilog' = [ilog EXCEPT ![n] = dlog[n]]
  /\ pos' = [pos EXCEPT ![n] = IF Becomes(n) THEN rec.term ELSE 0]
  /\ inh' = [inh EXCEPT ![n] = IF Becomes(n) /\ ~Pending(n) THEN Len(dlog[n]) ELSE 0]
  /\ pack' = [pack EXCEPT ![n] = 0]
  /\ lease' = [lease EXCEPT ![n] = 0]
  /\ pinc' = [pinc EXCEPT ![n] = IF Becomes(n) THEN [m \in Nodes |-> IF m = n THEN incn[n] ELSE NONE]
                                 ELSE NoRec]
  /\ viol' = IF Becomes(n) /\ ~\A e \in acked : e[3] >= r7T => Holds(n, e[1], e[2])
             THEN viol \cup {"promo"} ELSE viol
  /\ got' = [got EXCEPT ![n] = {}]
  /\ want' = [want EXCEPT ![n] = NOWANT]
  /\ lg' = [lg EXCEPT ![n] = IF @ \in {1, 2} /\ ~rs[n] THEN 0 ELSE @]
  /\ stopG' = [stopG EXCEPT ![n] = IF Abandons(n) THEN FALSE ELSE @]
  /\ act' = IF RecAct THEN <<"Learn", n>> ELSE <<>>
  /\ UNCHANGED <<rec, dlog, intent, acked, nid, crashes, rs, ph, src, rst, booted, incn, nonce, gr, msgs, gp,
                 req, rep, reseeds, wipes, r7T, seen>>

(* --- reconciliation as messages (R6.2, R6.3', LG-4) ------------------- *)
ReqSend(k) ==
  /\ ~isPrim[k] /\ pos[k] # T[k] /\ ~rs[k] /\ ~Pending(k) /\ want[k] = NOWANT /\ booted[k]
  /\ lg[k] = 0 /\ Cardinality(req) < MaxReqs
  /\ LET q == [from |-> k, inc |-> incn[k], term |-> T[k], blt |-> LastT(dlog[k]), ble |-> Len(dlog[k])]
     IN q \notin req /\ req' = req \cup {q}
  /\ act' = IF RecAct THEN <<"ReqSend", k>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph,
                 src, rst, booted, incn, nonce, gr, msgs, got, pinc, sv, gp, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

ReqDrop(q) ==
  /\ q \in req
  /\ req' = req \ {q}
  /\ act' = IF RecAct THEN <<"ReqDrop", q>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph,
                 src, rst, booted, incn, nonce, gr, msgs, got, pinc, sv, gp, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

Answer(j, q) ==
  /\ q \in req /\ Serving(j) /\ j # q.from /\ T[j] = q.term /\ Cardinality(rep[q.from]) < MaxReps
  /\ LET k == q.from
         ts == Max({x \in Terms(ilog[j]) : x <= q.blt})
         Lj == EndLE(ilog[j], ts)
         done == ts = q.blt /\ q.ble <= Lj
     IN /\ rep' = [rep EXCEPT ![k] = @ \cup {[tag |-> T[j], ts |-> ts, Lj |-> Lj, blt |-> q.blt,
                                              ble |-> q.ble, inc |-> q.inc]}]
        /\ pinc' = [pinc EXCEPT ![j][k] = IF done THEN q.inc ELSE IF q.inc # @ THEN NONE ELSE @]
        /\ seen' = IF q.inc # incn[k] THEN seen \cup {"late"} ELSE seen
  /\ act' = IF RecAct THEN <<"Answer", j, q>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph,
                 src, rst, booted, incn, nonce, gr, msgs, got, sv, gp, req, want, stopG, lg, reseeds, wipes,
                 r7T, viol>>

RecvRep(k, r) ==
  /\ r \in rep[k] /\ r.tag = T[k] /\ r.inc = incn[k] /\ ~isPrim[k] /\ pos[k] # T[k] /\ ~rs[k]
  /\ ~Pending(k) /\ want[k] = NOWANT /\ booted[k] /\ lg[k] = 0
  /\ r.blt = LastT(dlog[k]) /\ r.ble = Len(dlog[k])
  /\ IF r.ts = LastT(dlog[k]) /\ Len(dlog[k]) <= r.Lj
     THEN /\ pos' = [pos EXCEPT ![k] = T[k]]
          /\ stopG' = [stopG EXCEPT ![k] = FALSE]
          /\ UNCHANGED want
     ELSE /\ want' = [want EXCEPT ![k] = [L |-> Min({r.Lj, EndLE(dlog[k], r.ts)}), t |-> T[k],
                                          blt |-> r.blt, ble |-> r.ble]]
          /\ stopG' = [stopG EXCEPT ![k] = TRUE]
          /\ UNCHANGED pos
  /\ act' = IF RecAct THEN <<"RecvRep", k, r>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, intent, acked, nid, crashes, inh, pack, lease, rs, ph, src,
                 rst, booted, incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, lg, reseeds, wipes, r7T,
                 viol, seen>>

DropRep(k, r) ==
  /\ r \in rep[k]
  /\ rep' = [rep EXCEPT ![k] = @ \ {r}]
  /\ act' = IF RecAct THEN <<"DropRep", k, r>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph,
                 src, rst, booted, incn, nonce, gr, msgs, got, pinc, sv, gp, req, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

MarkTrunc(k) ==
  /\ want[k] # NOWANT /\ (FixWaitT => Waited(k))
  /\ T[k] = want[k].t /\ LastT(dlog[k]) = want[k].blt /\ Len(dlog[k]) = want[k].ble
  /\ ~isPrim[k] /\ ~Pending(k)
  /\ intent' = [intent EXCEPT ![k] = [L |-> want[k].L, t |-> T[k]]]
  /\ incn' = [incn EXCEPT ![k] = IF FixGenI THEN <<@[1], @[2] + 1>> ELSE @]
  /\ want' = [want EXCEPT ![k] = NOWANT]
  /\ act' = IF RecAct THEN <<"MarkTrunc", k>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, acked, nid, crashes, inh, pack, lease, rs, ph, src, rst,
                 booted, nonce, gr, msgs, got, pinc, sv, gp, req, rep, stopG, lg, reseeds, wipes, r7T, viol,
                 seen>>

ApplyIntent(k) ==
  /\ Pending(k) /\ Len(dlog[k]) > intent[k].L
  /\ dlog' = [dlog EXCEPT ![k] = SubSeq(@, 1, intent[k].L)]
  /\ ilog' = [ilog EXCEPT ![k] = SubSeq(@, 1, Min({intent[k].L, Len(@)}))]
  /\ act' = IF RecAct THEN <<"ApplyIntent", k>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph, src, rst,
                 booted, incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds, wipes,
                 r7T, viol, seen>>

ClearIntent(k) ==
  /\ Pending(k) /\ Len(dlog[k]) <= intent[k].L
  /\ intent' = [intent EXCEPT ![k] = NOINT]
  /\ inh' = [inh EXCEPT ![k] = IF isPrim[k] THEN Len(dlog[k]) ELSE @]
  /\ act' = IF RecAct THEN <<"ClearIntent", k>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, acked, nid, crashes, pack, lease, rs, ph, src, rst,
                 booted, incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds, wipes,
                 r7T, viol, seen>>

(* --- leaving S (LG-5), reseed in place, wipe, replaced disk ----------- *)
WaitOK(k) == CASE lg[k] = 1 -> (FixWaitR /\ FixOrder) => Waited(k)
             []   lg[k] = 4 -> FixWaitW => Waited(k)
             []   lg[k] = 6 -> FixWaitW => Waited(k)

Leave(k) ==
  /\ lg[k] \in {1, 4, 6} /\ WaitOK(k) /\ k \notin rec.S
  /\ IF FixLG5 THEN Chg(rec) ELSE UNCHANGED <<rec, gp>>
  /\ lg' = [lg EXCEPT ![k] = CASE @ = 1 -> 2 [] @ = 4 -> 5 [] @ = 6 -> 0]
  /\ seen' = IF FixLG5 /\ gp # {} THEN seen \cup {"bumpkill"} ELSE seen
  /\ act' = IF RecAct THEN <<"Leave", k>> ELSE <<>>
  /\ UNCHANGED <<T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, ph, src,
                 rst, booted, incn, nonce, gr, msgs, got, pinc, sv, req, rep, want, stopG, reseeds, wipes,
                 r7T, viol>>

RS0(k) ==
  /\ reseeds < MaxReseeds /\ k \notin rec.S /\ ~isPrim[k] /\ ~rs[k] /\ lg[k] = 0 /\ ~Pending(k)
  /\ want[k] = NOWANT /\ booted[k]
  /\ reseeds' = reseeds + 1
  /\ lg' = [lg EXCEPT ![k] = 1]
  /\ stopG' = [stopG EXCEPT ![k] = TRUE]
  /\ lease' = [lease EXCEPT ![k] = 0]
  /\ act' = IF RecAct THEN <<"RS0", k>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, rs, ph, src,
                 rst, booted, incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want, wipes, r7T, viol,
                 seen>>

RS1(k) ==
  /\ lg[k] = 2 /\ ~rs[k] /\ ~isPrim[k] /\ ~Pending(k) /\ (FixWaitR => Waited(k))
  /\ rs' = [rs EXCEPT ![k] = TRUE]
  /\ ph' = [ph EXCEPT ![k] = 1]
  /\ incn' = [incn EXCEPT ![k] = IF FixGenR THEN <<@[1], @[2] + 1>> ELSE @]
  /\ pos' = [pos EXCEPT ![k] = 0]
  /\ act' = IF RecAct THEN <<"RS1", k>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, intent, acked, nid, crashes, inh, pack, lease, src, rst,
                 booted, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds, wipes, r7T,
                 viol, seen>>

RS2(k) ==
  /\ rs[k] /\ ph[k] = 1
  /\ dlog' = [dlog EXCEPT ![k] = <<>>]
  /\ ilog' = [ilog EXCEPT ![k] = <<>>]
  /\ ph' = [ph EXCEPT ![k] = 2]
  /\ act' = IF RecAct THEN <<"RS2", k>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, src, rst, booted,
                 incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds, wipes, r7T,
                 viol, seen>>

RS3(k, j) ==
  /\ rs[k] /\ ph[k] = 2 /\ j # k /\ Serving(j) /\ T[j] >= T[k]
  /\ T' = [T EXCEPT ![k] = T[j]]
  /\ src' = [src EXCEPT ![k] = j]
  /\ rst' = [rst EXCEPT ![k] = T[j]]
  /\ ph' = [ph EXCEPT ![k] = 3]
  /\ act' = IF RecAct THEN <<"RS3", k, j>> ELSE <<>>
  /\ UNCHANGED <<rec, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, booted,
                 incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds, wipes, r7T,
                 viol, seen>>

RS5(k, n) ==
  /\ rs[k] /\ ph[k] = 3 /\ isPrim[src[k]] /\ T[src[k]] = rst[k] /\ T[k] = rst[k]
  /\ n \in 0..Len(ilog[src[k]])
  /\ dlog' = [dlog EXCEPT ![k] = SubSeq(ilog[src[k]], 1, n)]
  /\ ilog' = [ilog EXCEPT ![k] = SubSeq(ilog[src[k]], 1, n)]
  /\ ph' = [ph EXCEPT ![k] = 4]
  /\ act' = IF RecAct THEN <<"RS5", k, n>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, src, rst, booted,
                 incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds, wipes, r7T,
                 viol, seen>>

RS6(k) ==
  /\ rs[k] /\ ph[k] = 4 /\ T[k] = rst[k]
  /\ rs' = [rs EXCEPT ![k] = FALSE]
  /\ ph' = [ph EXCEPT ![k] = 0]
  /\ lg' = [lg EXCEPT ![k] = 0]
  /\ pos' = [pos EXCEPT ![k] = 0]
  /\ seen' = IF \E r \in msgs : r.from = k /\ r.l > Len(dlog[k]) THEN seen \cup {"short"} ELSE seen
  /\ act' = IF RecAct THEN <<"RS6", k>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, intent, acked, nid, crashes, inh, pack, lease, src, rst,
                 booted, incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want, stopG, reseeds, wipes,
                 r7T, viol>>

RSBack(k) ==
  /\ rs[k] /\ ph[k] \in {3, 4}
  /\ ~(T[k] = rst[k] /\ (ph[k] = 4 \/ (isPrim[src[k]] /\ T[src[k]] = rst[k])))
  /\ ph' = [ph EXCEPT ![k] = 2]
  /\ act' = IF RecAct THEN <<"RSBack", k>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, pos, intent, acked, nid, crashes, inh, pack, lease, rs, src,
                 rst, booted, incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, want, stopG, lg, reseeds,
                 wipes, r7T, viol, seen>>

WipeStop(k) ==
  /\ wipes < MaxWipes /\ lg[k] = 0 /\ ~isPrim[k] /\ ~rs[k]
  /\ wipes' = wipes + 1
  /\ lg' = [lg EXCEPT ![k] = 4]
  /\ booted' = [booted EXCEPT ![k] = FALSE]
  /\ stopG' = [stopG EXCEPT ![k] = TRUE]
  /\ pos' = [pos EXCEPT ![k] = 0]
  /\ want' = [want EXCEPT ![k] = NOWANT]
  /\ lease' = [lease EXCEPT ![k] = 0]
  /\ act' = IF RecAct THEN <<"WipeStop", k>> ELSE <<>>
  /\ UNCHANGED <<rec, T, isPrim, dlog, ilog, A, intent, acked, nid, crashes, inh, pack, rs, ph, src, rst,
                 incn, nonce, gr, msgs, got, pinc, sv, gp, req, rep, reseeds, r7T, viol, seen>>

WipeDo(k, j, n) ==
  /\ lg[k] = 5 /\ (FixWaitW => Waited(k))
  /\ j # k /\ isPrim[j] /\ n \in 0..Len(ilog[j])
  /\ dlog' = [dlog EXCEPT ![k] = SubSeq(ilog[j], 1, n)]
  /\ ilog' = [ilog EXCEPT ![k] = SubSeq(ilog[j], 1, n)]
  /\ T' = [T EXCEPT ![k] = T[j]]
  /\ nonce' = IF FixNonce THEN nonce + 1 ELSE nonce
  /\ incn' = [incn EXCEPT ![k] = <<IF FixNonce THEN nonce + 1 ELSE @[1], 1>>]
  /\ intent' = [intent EXCEPT ![k] = NOINT]
  /\ rep' = [rep EXCEPT ![k] = {}]
  /\ lg' = [lg EXCEPT ![k] = 0]
  /\ stopG' = [stopG EXCEPT ![k] = FALSE]
  /\ seen' = seen \cup {"wipe"}
  /\ act' = IF RecAct THEN <<"WipeDo", k, j, n>> ELSE <<>>
  /\ UNCHANGED <<rec, isPrim, A, pos, acked, nid, crashes, inh, pack, lease, rs, ph, src, rst, booted, gr,
                 msgs, got, pinc, sv, gp, req, want, reseeds, wipes, r7T, viol>>

Replace(k) ==
  /\ Repl /\ wipes < MaxWipes /\ lg[k] = 0 /\ ~rs[k] /\ Waited(k)
  /\ wipes' = wipes + 1
  /\ dlog' = [dlog EXCEPT ![k] = <<>>]
  /\ ilog' = [ilog EXCEPT ![k] = <<>>]
  /\ T' = [T EXCEPT ![k] = rec.term]
  /\ nonce' = nonce + 1
  /\ incn' = [incn EXCEPT ![k] = <<nonce + 1, 1>>]
  /\ isPrim' = [isPrim EXCEPT ![k] = FALSE]
  /\ A' = [A EXCEPT ![k] = {}]
  /\ sv' = [sv EXCEPT ![k] = {}]
  /\ pos' = [pos EXCEPT ![k] = 0]
  /\ intent' = [intent EXCEPT ![k] = NOINT]
  /\ lease' = [lease EXCEPT ![k] = 0]
  /\ pinc' = [pinc EXCEPT ![k] = NoRec]
  /\ got' = [got EXCEPT ![k] = {}]
  /\ rep' = [rep EXCEPT ![k] = {}]
  /\ want' = [want EXCEPT ![k] = NOWANT]
  /\ lg' = [lg EXCEPT ![k] = 6]
  /\ stopG' = [stopG EXCEPT ![k] = TRUE]
  /\ seen' = seen \cup {"repl"}
  /\ act' = IF RecAct THEN <<"Replace", k>> ELSE <<>>
  /\ UNCHANGED <<rec, acked, nid, crashes, inh, pack, rs, ph, src, rst, booted, gr, msgs, gp, req, reseeds,
                 r7T, viol>>

(* --- crash and recovery ----------------------------------------------- *)
Crash(n) ==
  /\ crashes < MaxCrashes /\ lg[n] \notin Down
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
  /\ got' = [got EXCEPT ![n] = {}]
  /\ booted' = [booted EXCEPT ![n] = FALSE]
  /\ ph' = [ph EXCEPT ![n] = IF rs[n] THEN 1 ELSE 0]
  /\ incn' = [incn EXCEPT ![n] = IF FixGenB THEN <<@[1], @[2] + 1>> ELSE @]
  /\ want' = [want EXCEPT ![n] = NOWANT]
  /\ lg' = [lg EXCEPT ![n] = IF (@ \in {1, 2} /\ ~rs[n]) \/ (@ = 6 /\ ~FixFreshDur) THEN 0 ELSE @]
  /\ stopG' = [stopG EXCEPT ![n] = rs[n] \/ (lg[n] = 6 /\ FixFreshDur)]
  /\ seen' = IF rs[n] THEN seen \cup {"crashrs"} ELSE seen
  /\ act' = IF RecAct THEN <<"Crash", n>> ELSE <<>>
  /\ UNCHANGED <<rec, T, acked, nid, inh, pack, rs, src, rst, nonce, gr, msgs, gp, req, rep, reseeds, wipes,
                 r7T, viol>>

Step ==
  \/ \E p \in Nodes : NewEntry(p) \/ WriteLocal(p) \/ Promote(p) \/ OpOutside(p) \/ Learn(p)
                      \/ MarkReady(p) \/ ApplyIntent(p) \/ ClearIntent(p) \/ Crash(p) \/ LExpire(p)
                      \/ LearnS(p) \/ BootRead(p) \/ Send(p) \/ ReqSend(p) \/ MarkTrunc(p)
                      \/ Leave(p) \/ RS0(p) \/ RS1(p) \/ RS2(p) \/ RS6(p) \/ RSBack(p)
                      \/ WipeStop(p) \/ Replace(p)
  \/ \E p, m \in Nodes : AddA(p, m) \/ ProposeGrow(p, m) \/ Shrink(p, m) \/ Drop(p, m)
                         \/ SGrant(p, m) \/ Grant(m, p) \/ RS3(m, p)
  \/ \E p, m \in Nodes, i \in 1..MaxIds : Ship(p, m, i)
  \/ \E p \in Nodes, i \in 1..MaxIds : Ack(p, i)
  \/ \E k \in Nodes, n \in 0..MaxIds : RS5(k, n)
  \/ \E k, j \in Nodes, n \in 0..MaxIds : WipeDo(k, j, n)
  \/ \E k \in Nodes : \E x \in gr[k] : Expire(k, x)
  \/ \E k \in Nodes : \E r \in rep[k] : RecvRep(k, r) \/ DropRep(k, r)
  \/ \E q \in req : ReqDrop(q) \/ \E j \in Nodes : Answer(j, q)
  \/ \E r \in msgs : DropMsg(r) \/ \E p \in Nodes : Deliver(p, r)
  \/ \E x \in gp : ChooseGrow(x)

Held(l) == UNION {{<<i, l[n][i]>> : i \in 1..Len(l[n])} : n \in Nodes}
Next == Step /\ hist' = hist \cup Held(dlog') \cup Held(ilog')

Spec == Init /\ [][Next]_vars
Sym == Permutations(Nodes \ {P0})
Shallow == TLCGet("level") <= 20
Shallow16 == TLCGet("level") <= 16

(* --- properties ------------------------------------------------------- *)
AckSound == "ack" \notin viol
GrowSound == "grow" \notin viol
LeaseSound == "lease" \notin viol
PromoSound == "promo" \notin viol
AckedPreserved == \A e \in acked : e[3] >= r7T => \A n \in rec.S : Holds(n, e[1], e[2])
EntryUnique == \A a, b \in Nodes : \A i \in 1..Min({Len(dlog[a]), Len(dlog[b])}) :
                 dlog[a][i].t = dlog[b][i].t => dlog[a][i] = dlog[b][i]
HistoryUnique == \A x, y \in hist : (x[1] = y[1] /\ x[2].t = y[2].t) => x[2] = y[2]
LogMatching == \A a, b \in Nodes : \A i \in 1..Min({Len(dlog[a]), Len(dlog[b])}) :
                 dlog[a][i] = dlog[b][i] => SubSeq(dlog[a], 1, i) = SubSeq(dlog[b], 1, i)
OutsideS == \A k \in Nodes : (lg[k] \in {2, 5} \/ rs[k]) =>
              k \notin rec.S \/ (k = rec.prim /\ rec.term = r7T /\ ~isPrim[k])
Core == AckSound /\ LeaseSound /\ PromoSound /\ EntryUnique /\ LogMatching /\ HistoryUnique /\ OutsideS
Safety == Core /\ GrowSound /\ AckedPreserved

(* --- reachability probes: TLC must report each one violated ---------- *)
NoRejectByInc == "inc" \notin seen
NoLateRequest == "late" \notin seen
NoBumpKill == "bumpkill" \notin seen
NoWaitBlock == \A k \in Nodes : ~(want[k] # NOWANT /\ ~Waited(k))
NoShortReseed == "short" \notin seen
NoWipe == "wipe" \notin seen
NoReplace == "repl" \notin seen
NoCrashInReseed == "crashrs" \notin seen
NoAckAfterR7 == "ackr7" \notin seen
=============================================================================
