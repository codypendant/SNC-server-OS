------------------------------ MODULE SegChain ------------------------------
(* MetaWatermark revision 5: R-CHAIN across one WAL stream's segment         *)
(* lifecycle (docs/AFR_001_D02_MODEL.md section 18). It reuses part wm's     *)
(* abstractions: entry = <<term, lsn>>, inode index = segment nonce,         *)
(* directory operations pending until a directory fsync, one media fault,    *)
(* scan with the void rule by name, FL-chain, R-CHAIN, watermark and OD-2    *)
(* binding checks. Unlike part wm, recovery is not terminal: after an        *)
(* accepted recovery the writer reopens E + 1 (design 3(b), 3(c)) and runs on. *)
(*                                                                            *)
(* AREA selects one narrow lane set:                                          *)
(*  "life"  rollover (open_segment), truncation, reopen; directory faults     *)
(*  "spare" a prepared spare taken at rollover, in order Order; adoption      *)
(*  "c15"   3(b) replacement that keeps the nonce; a lost format write        *)
(*  "seed"  a per-stream reseed from an empty directory (C-S), then appends   *)
(*                                                                            *)
(* Abstractions: a truncation (completion) and a reseed are one step with one *)
(* directory fsync; crashes inside completion are part wm's job (revisions    *)
(* 1-4), crashes inside a seed are boundary rule 5. A replacement's rename is *)
(* never lost. Names below the floor are not read (O-FLOOR). Watermark and    *)
(* data-write faults are part wm's. One stream, no wire, no reclaim.          *)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS AREA, MUT, MaxLsn, MaxCrash, MaxTrunc, FaultOn,
          Order     \* spare: "Code" (header rides in the first flush), "RenameFirst", "HeaderFirst"

VARIABLES ino,      \* nonce -> [first (0 = unnamed prepared header), prev, pn, recs]
          nIno,
          dirDur,   \* durable directory: name -> nonce; name 0 is spare.tmp
          dirMem,
          pend,     \* directory operations not yet fsynced (only a spare rename)
          wm,       \* [lsn, e, bn, nonce]: watermark and OD-2 binding of name bn
          wIno,     \* writer's segment
          hdrPend,  \* Code order: the spare header goes out with the next data write
          hv,       \* the spare's take-time header [first, prev, pn]
          spSt,     \* "none" "ready" "hdr" "renamed" "taken"
          dPend,    \* data write in flight (OD-1: at most one) or NoD
          intended, \* ghost: the node's current log
          acked,    \* ghost: acknowledged <<x, e>>
          vt, ltr,  \* ghost: tail voided by the last truncation or seed, and its base
          term, ntr, floor, ckpt, nseed,
          phase,    \* "run" "rec" "chk" "done"
          outcome,  \* [st, log, ign]
          fault, crashes,
          ev, nroll \* ghost: events seen, rollovers done

vars == <<ino, nIno, dirDur, dirMem, pend, wm, wIno, hdrPend, hv, spSt, dPend, intended, acked,
          vt, ltr, term, ntr, floor, ckpt, nseed, phase, outcome, fault, crashes, ev, nroll>>

MutOk == MUT \in {"none", "NoChain", "ConstPn", "PnZero", "CmpMinus1", "LastOnly", "AboveL1",
                  "NoFL", "NoFLLastOnly", "NoFLAboveL1", "NoNonceCheck", "UnnamedDamage",
                  "SparePnAtPrepare", "NoFrameNonce", "ReplaceNewNonce", "SeedNoClear",
                  "SeedKeepBind", "SeedNoBind"}

Nm == 0 .. MaxLsn + 1
EMPTY == <<0, 0>>   \* format sectors of the segment's own nonce
TORN  == <<0, 1>>   \* a torn frame: part frame, part format sectors
DMG   == <<0, 2>>   \* damage: not a frame or format sector of this nonce
Valid(r) == r[1] > 0
Term(e) == e[1]
Old(x) == <<1, x>>
NoD == [x |-> 0, e |-> EMPTY]
EmptyRecs == [x \in 1 .. MaxLsn |-> EMPTY]
SP == 3             \* the spare's nonce (spare lane)
Max(S) == CHOOSE m \in S : \A k \in S : k <= m
FK == IF AREA = "c15" THEN {"format"} ELSE {"dir"}

ApplyOp(d, op) == IF op.mv THEN [d EXCEPT ![0] = 0, ![op.name] = op.nonce]
                  ELSE [d EXCEPT ![op.name] = op.nonce]          \* nonce 0 = unlink
ApplySeq(d, s) == LET F[i \in 0 .. Len(s)] == IF i = 0 THEN d ELSE ApplyOp(F[i - 1], s[i])
                  IN F[Len(s)]
Drop(s, i) == SubSeq(s, 1, i - 1) \o SubSeq(s, i + 1, Len(s))
PresentM(d) == {n \in 1 .. MaxLsn + 1 : d[n] # 0}
SegOfD(d, x) == IF \E n \in PresentM(d) : n <= x THEN Max({n \in PresentM(d) : n <= x}) ELSE 0
Unl(del) == [i \in 1 .. Cardinality(del) |->
               [name |-> CHOOSE n \in del : Cardinality({m \in del : m > n}) = i - 1,
                nonce |-> 0, mv |-> FALSE]]

\* A directory fsync of ops: all land, or (the one fault) one of them is lost.
Sync(ops) ==
  \/ dirDur' = ApplySeq(dirDur, ops) /\ fault' = fault
  \/ /\ FaultOn /\ "dir" \in FK /\ fault = "none" /\ Len(ops) > 0
     /\ \E i \in 1 .. Len(ops) :
          /\ dirDur' = ApplySeq(dirDur, Drop(ops, i))
          /\ fault' = (IF ops[i].nonce = 0 THEN "lostUnlink" ELSE "lostBind")

\* The writer's link (R-CHAIN assignment) under the design or a mutation.
PnOf(w) == CASE MUT = "ConstPn" -> 1 [] MUT = "PnZero" -> 0 [] OTHER -> w

\* ------------------------------ recovery ----------------------------------
Fail == [ok |-> FALSE, log |-> <<>>, ign |-> FALSE]
Ref  == [st |-> "refused", log |-> <<>>, ign |-> FALSE]

Scan(d) ==
  LET P == {n \in 1 .. MaxLsn + 1 : d[n] # 0 /\ n >= floor}
      I(n) == ino[d[n]]
      R(n) == I(n).recs
      \* the newest name holding an unnamed prepared header and no valid frame is
      \* unwritten (today's all-zero rule, storage.cpp:379); UnnamedDamage refuses it
      Unwr(n) == I(n).first = 0 /\ ~\E x \in 1 .. MaxLsn : Valid(R(n)[x])
      ign == P # {} /\ Unwr(Max(P)) /\ MUT # "UnnamedDamage"
      Q == IF ign THEN P \ {Max(P)} ELSE P
      Nx(n) == IF \E m \in Q : m > n THEN CHOOSE m \in Q : m > n /\ \A k \in Q : k > n => k >= m
               ELSE 0
      Pr(n) == Max({m \in Q : m < n})
      SegQ(x) == Max({n \in Q : n <= x})
      RunEnd(n, lim) == LET Rr == {x \in n .. MaxLsn : x < lim /\ \A y \in n .. x : Valid(R(n)[y])}
                        IN IF Rr = {} THEN n - 1 ELSE Max(Rr)
      Last == Max(Q)
      E == RunEnd(Last, MaxLsn + 1)
      Named == \A n \in Q : I(n).first = n
      GapFree == \A n \in Q : Nx(n) # 0 => RunEnd(n, Nx(n)) = Nx(n) - 1
      \* above the end only format sectors or a torn frame may be discarded
      TailOK == \A y \in (E + 1) .. MaxLsn : R(Last)[y] \in {EMPTY, TORN}
      ChkR(n) == CASE MUT \in {"LastOnly", "NoFLLastOnly"} -> n = Last
                   [] MUT \in {"AboveL1", "NoFLAboveL1"} -> n > wm.bn
                   [] OTHER -> TRUE
      PnOK(n) == CASE MUT = "NoChain"   -> TRUE
                   [] MUT = "CmpMinus1" -> I(n).pn = d[n - 1]
                   [] OTHER             -> I(n).pn = d[Pr(n)]
      FLOK(n) == MUT \in {"NoFL", "NoFLLastOnly", "NoFLAboveL1"}
                 \/ I(n).prev = Term(R(SegQ(n - 1))[n - 1])
      \* FL-chain and R-CHAIN on every retained segment after the first
      Chain == \A n \in Q : n > floor => FLOK(n) /\ (ChkR(n) => PnOK(n))
      LogE == [x \in 1 .. E |-> IF x < floor THEN ckpt[x] ELSE R(SegQ(x))[x]]
  IN IF Q = {} \/ floor \notin Q \/ ~Named THEN Fail
     ELSE IF ~GapFree \/ ~TailOK \/ ~Chain THEN Fail
     ELSE [ok |-> TRUE, log |-> LogE, ign |-> ign]

Check(d) ==
  LET s == Scan(d)
  IN IF ~s.ok THEN Ref
     ELSE IF wm.lsn > Len(s.log) THEN Ref
     ELSE IF wm.lsn >= 1 /\ s.log[wm.lsn] # wm.e THEN Ref
     ELSE IF MUT # "NoNonceCheck" /\ wm.nonce # 0 /\ wm.bn >= floor /\ d[wm.bn] # wm.nonce THEN Ref
     ELSE [st |-> "ok", log |-> s.log, ign |-> s.ign]

\* -------------------------------- init ------------------------------------
\* Layout A of part wm: segment 1 holds LSN 1..2, segment 3 holds LSN 3; all acknowledged.
Init ==
  /\ MutOk
  /\ ino = LET s1 == [first |-> 1, prev |-> 0, pn |-> 0,
                      recs |-> [x \in 1 .. MaxLsn |-> IF x <= 2 THEN Old(x) ELSE EMPTY]]
               s3 == [first |-> 3, prev |-> 1, pn |-> 1,
                      recs |-> [x \in 1 .. MaxLsn |-> IF x = 3 THEN Old(x) ELSE EMPTY]]
               sp == [first |-> 0, prev |-> 0, pn |-> 0, recs |-> EmptyRecs]
           IN IF AREA = "spare" THEN <<s1, s3, sp>> ELSE <<s1, s3>>
  /\ nIno = IF AREA = "spare" THEN 3 ELSE 2
  /\ dirDur = [n \in Nm |-> CASE n = 1 -> 1 [] n = 3 -> 2
                             [] n = 0 /\ AREA = "spare" -> SP [] OTHER -> 0]
  /\ dirMem = dirDur /\ pend = <<>>
  /\ wm = [lsn |-> 3, e |-> Old(3), bn |-> 0, nonce |-> 0]
  /\ wIno = 2 /\ hdrPend = FALSE /\ hv = [first |-> 0, prev |-> 0, pn |-> 0]
  /\ spSt = (IF AREA = "spare" THEN "ready" ELSE "none")
  /\ dPend = NoD
  /\ intended = <<Old(1), Old(2), Old(3)>>
  /\ acked = {<<1, Old(1)>>, <<2, Old(2)>>, <<3, Old(3)>>}
  /\ vt = [x \in 1 .. MaxLsn |-> EMPTY] /\ ltr = 0 /\ term = 1 /\ ntr = 0
  /\ floor = 1 /\ ckpt = <<>> /\ nseed = 0
  /\ phase = "run" /\ outcome = [st |-> "none", log |-> <<>>, ign |-> FALSE]
  /\ fault = "none" /\ crashes = 0 /\ ev = {} /\ nroll = 0

\* ------------------------------- writer -----------------------------------
Mid == spSt \in {"hdr", "renamed"}   \* a spare take is half done
Quiet == phase = "run" /\ dPend = NoD /\ ~hdrPend /\ ~Mid

\* A new entry is the current term's, or (V4, log matching) the identical entry of the
\* voided tail while everything since the truncation is identical too.
ReApp(x) == IF vt[x] # EMPTY /\ \A y \in (ltr + 1) .. (x - 1) : intended[y] = vt[y]
            THEN {vt[x]} ELSE {}

AppData == phase = "run" /\ dPend = NoD /\ ~Mid /\ Len(intended) < MaxLsn /\
  LET x == Len(intended) + 1
  IN \E e \in {<<term, x>>} \cup ReApp(x) :
       /\ dPend' = [x |-> x, e |-> e] /\ intended' = Append(intended, e)
       /\ UNCHANGED <<ino, nIno, dirDur, dirMem, pend, wm, wIno, hdrPend, hv, spSt, acked, vt,
                      ltr, term, ntr, floor, ckpt, nseed, phase, outcome, fault, crashes, ev, nroll>>

WithHdr(f, h) == IF h THEN [f EXCEPT !.first = hv.first, !.prev = hv.prev, !.pn = hv.pn] ELSE f

DataLand == phase = "run" /\ dPend # NoD
  /\ ino' = [ino EXCEPT ![wIno] = [WithHdr(@, hdrPend) EXCEPT !.recs[dPend.x] = dPend.e]]
  /\ dPend' = NoD /\ hdrPend' = FALSE
  /\ UNCHANGED <<nIno, dirDur, dirMem, pend, wm, wIno, hv, spSt, intended, acked, vt, ltr, term,
                 ntr, floor, ckpt, nseed, phase, outcome, fault, crashes, ev, nroll>>

\* The watermark follows the data; it carries the binding (OD-2); then the ack.
AppWm == phase = "run" /\ wm.lsn < Len(intended) /\ dPend.x # wm.lsn + 1 /\
  LET x == wm.lsn + 1
  IN /\ wm' = [wm EXCEPT !.lsn = x, !.e = intended[x]]
     /\ acked' = acked \cup {<<x, intended[x]>>}
     /\ UNCHANGED <<ino, nIno, dirDur, dirMem, pend, wIno, hdrPend, hv, spSt, dPend, intended,
                    vt, ltr, term, ntr, floor, ckpt, nseed, phase, outcome, fault, crashes, ev, nroll>>

\* Rollover is allowed once the writer's segment holds a record.
RollOk == Quiet /\ Len(intended) < MaxLsn /\ Len(intended) >= ino[wIno].first
          /\ ino[wIno].first > 0 /\ dirMem[Len(intended) + 1] = 0
Roll2 == IF nroll + 1 >= 2 THEN ev \cup {"roll2"} ELSE ev

\* open_segment by design 3(a): TMP with header, fdatasync, rename, fsync dir.
OpenSeg == RollOk /\
  LET x == Len(intended) + 1
      n == nIno + 1
  IN /\ ino' = Append(ino, [first |-> x, prev |-> Term(intended[x - 1]), pn |-> PnOf(wIno),
                            recs |-> EmptyRecs])
     /\ nIno' = n /\ dirMem' = [dirMem EXCEPT ![x] = n]
     /\ Sync(pend \o <<[name |-> x, nonce |-> n, mv |-> FALSE]>>) /\ pend' = <<>>
     /\ wIno' = n /\ nroll' = nroll + 1 /\ ev' = Roll2
     /\ UNCHANGED <<wm, hdrPend, hv, spSt, dPend, intended, acked, vt, ltr, term, ntr, floor,
                    ckpt, nseed, phase, outcome, crashes>>

SpHdr(x) == [first |-> x, prev |-> Term(intended[x - 1]),
             pn |-> (IF MUT = "SparePnAtPrepare" THEN 2 ELSE PnOf(wIno))]

\* take_spare: rename spare.tmp to its name (no directory fsync). Code order: the
\* header rides in the first flush (storage.cpp:672). RenameFirst: the header is then
\* written by itself (DSYNC) before the first frame.
TakeRen == RollOk /\ spSt = "ready" /\ Order # "HeaderFirst" /\
  LET x == Len(intended) + 1
  IN /\ hv' = SpHdr(x)
     /\ dirMem' = [dirMem EXCEPT ![0] = 0, ![x] = SP]
     /\ pend' = Append(pend, [name |-> x, nonce |-> SP, mv |-> TRUE])
     /\ wIno' = SP
     /\ IF Order = "Code" THEN hdrPend' = TRUE /\ spSt' = "taken"
                          ELSE hdrPend' = FALSE /\ spSt' = "renamed"
     /\ nroll' = nroll + 1 /\ ev' = Roll2 \cup {"spare"}
     /\ UNCHANGED <<ino, nIno, dirDur, wm, dPend, intended, acked, vt, ltr, term, ntr, floor,
                    ckpt, nseed, phase, outcome, fault, crashes>>

TakeHdr == phase = "run" /\ spSt = "renamed"
  /\ ino' = [ino EXCEPT ![SP] = WithHdr(@, TRUE)] /\ spSt' = "taken"
  /\ UNCHANGED <<nIno, dirDur, dirMem, pend, wm, wIno, hdrPend, hv, dPend, intended, acked, vt,
                 ltr, term, ntr, floor, ckpt, nseed, phase, outcome, fault, crashes, ev, nroll>>

\* HeaderFirst: the take-time header is written to spare.tmp (DSYNC), then the rename.
HdrFirst == RollOk /\ spSt = "ready" /\ Order = "HeaderFirst" /\
  LET x == Len(intended) + 1
  IN /\ hv' = SpHdr(x)
     /\ ino' = [ino EXCEPT ![SP] = [@ EXCEPT !.first = x, !.prev = SpHdr(x).prev,
                                             !.pn = SpHdr(x).pn]]
     /\ spSt' = "hdr"
     /\ UNCHANGED <<nIno, dirDur, dirMem, pend, wm, wIno, hdrPend, dPend, intended, acked, vt,
                    ltr, term, ntr, floor, ckpt, nseed, phase, outcome, fault, crashes, ev, nroll>>

RenAfter == phase = "run" /\ spSt = "hdr" /\
  LET x == hv.first
  IN /\ dirMem' = [dirMem EXCEPT ![0] = 0, ![x] = SP]
     /\ pend' = Append(pend, [name |-> x, nonce |-> SP, mv |-> TRUE])
     /\ wIno' = SP /\ spSt' = "taken" /\ nroll' = nroll + 1 /\ ev' = Roll2 \cup {"spare"}
     /\ UNCHANGED <<ino, nIno, dirDur, hdrPend, hv, wm, dPend, intended, acked, vt, ltr, term,
                    ntr, floor, ckpt, nseed, phase, outcome, fault, crashes>>

\* A truncation to L2 (completion, one step): unlink names above L2, create L2 + 1 whole,
\* one directory fsync, lower the watermark with the binding (L2 + 1, nonce).
Trunc == Quiet /\ AREA \in {"life", "c15", "seed"} /\ ntr < MaxTrunc /\
  \E L2 \in floor .. Len(intended) :
    LET del == {m \in PresentM(dirMem) : m > L2 /\ m # L2 + 1}
        S == SegOfD(dirMem, L2)
        n == nIno + 1
    IN /\ ino' = Append(ino, [first |-> L2 + 1, prev |-> Term(intended[L2]),
                              pn |-> PnOf(dirMem[S]), recs |-> EmptyRecs])
       /\ nIno' = n
       /\ dirMem' = [m \in Nm |-> IF m \in del THEN 0 ELSE IF m = L2 + 1 THEN n ELSE dirMem[m]]
       /\ Sync(pend \o Unl(del) \o <<[name |-> L2 + 1, nonce |-> n, mv |-> FALSE]>>)
       /\ pend' = <<>>
       /\ wm' = [lsn |-> L2, e |-> intended[L2], bn |-> L2 + 1, nonce |-> n]
       /\ intended' = SubSeq(intended, 1, L2)
       /\ vt' = [x \in 1 .. MaxLsn |-> IF x > L2 /\ x <= Len(intended) THEN intended[x] ELSE EMPTY]
       /\ ltr' = L2 /\ term' = term + 1 /\ ntr' = ntr + 1
       /\ acked' = {a \in acked : a[1] <= L2}
       /\ wIno' = n
       /\ ev' = (IF ntr + 1 = 2 THEN ev \cup {"trunc2"} ELSE ev)
       /\ UNCHANGED <<hdrPend, hv, spSt, dPend, floor, ckpt, nseed, phase, outcome, crashes, nroll>>

\* A per-stream reseed under C-S: unlink every name, then the seed's first segment at
\* the new floor f (prev 0, pn 0: first retained, not checked). The donor log shares
\* the node's history up to f - 1, or holds new entries from the floor. The checkpoint
\* below f and the new floor are durable with the seed's record (one step).
Reseed == Quiet /\ AREA = "seed" /\ nseed = 0 /\
  \E f \in (floor + 1) .. MaxLsn : \E same \in BOOLEAN :
    LET old == intended
        dl == [x \in 1 .. f - 1 |-> IF x <= Len(old) /\ (same \/ x < floor) THEN old[x]
                                    ELSE <<term + 1, x>>]
        del == IF MUT = "SeedNoClear" THEN {} ELSE PresentM(dirMem) \ {f}
        n == nIno + 1
        keep == f - 1 <= Len(old) /\ dl[f - 1] = old[f - 1]
    IN /\ ino' = Append(ino, [first |-> f, prev |-> 0, pn |-> 0, recs |-> EmptyRecs])
       /\ nIno' = n
       /\ dirMem' = [m \in Nm |-> IF m \in del THEN 0 ELSE IF m = f THEN n ELSE dirMem[m]]
       /\ Sync(pend \o Unl(del) \o <<[name |-> f, nonce |-> n, mv |-> FALSE]>>)
       /\ pend' = <<>>
       /\ intended' = dl /\ ckpt' = dl /\ floor' = f /\ term' = term + 1
       /\ vt' = [x \in 1 .. MaxLsn |-> IF keep /\ x >= f /\ x <= Len(old) THEN old[x] ELSE EMPTY]
       /\ ltr' = f - 1 /\ acked' = {}
       /\ wm' = CASE MUT = "SeedKeepBind" -> [wm EXCEPT !.lsn = f - 1, !.e = dl[f - 1]]
                  [] MUT = "SeedNoBind"   -> [lsn |-> f - 1, e |-> dl[f - 1], bn |-> 0, nonce |-> 0]
                  [] OTHER                -> [lsn |-> f - 1, e |-> dl[f - 1], bn |-> f, nonce |-> n]
       /\ wIno' = n /\ nseed' = 1 /\ ev' = ev \cup {"seed"}
       /\ UNCHANGED <<hdrPend, hv, spSt, dPend, ntr, phase, outcome, crashes, nroll>>

\* A crash: the pending rename lands or not; the data write in flight is absent, torn or
\* whole; in Code order its header sector lands independently (a torn write).
Crash ==
  /\ phase = "run" /\ crashes < MaxCrash
  /\ \/ dirDur' = dirDur
     \/ dirDur' = ApplySeq(dirDur, pend)
  /\ IF dPend = NoD THEN ino' = ino
     ELSE \E r \in {ino[wIno].recs[dPend.x], TORN, dPend.e} :
          \E h \in (IF hdrPend THEN BOOLEAN ELSE {FALSE}) :
            ino' = [ino EXCEPT ![wIno] = [WithHdr(@, h) EXCEPT !.recs[dPend.x] = r]]
  /\ pend' = <<>> /\ dPend' = NoD /\ hdrPend' = FALSE
  /\ spSt' = (IF spSt = "ready" THEN "ready" ELSE "none")
  /\ phase' = "rec" /\ crashes' = crashes + 1
  /\ UNCHANGED <<nIno, dirMem, wm, wIno, hv, intended, acked, vt, ltr, term, ntr, floor, ckpt,
                 nseed, outcome, fault, ev, nroll>>

\* Recovery: adopt spare.tmp by its header (RENAME_NOREPLACE: refuse if the name exists;
\* an unnamed spare holding a valid frame refuses; else it is unlinked), then scan and check.
Recover == phase = "rec" /\
  LET d0 == dirDur
      i == d0[0]
      named == i # 0 /\ ino[i].first > 0
      refA == i # 0 /\ IF named THEN d0[ino[i].first] # 0
                       ELSE \E x \in 1 .. MaxLsn : Valid(ino[i].recs[x])
      d1 == IF i = 0 THEN d0
            ELSE IF named THEN [d0 EXCEPT ![0] = 0, ![ino[i].first] = i] ELSE [d0 EXCEPT ![0] = 0]
      o == IF refA THEN Ref ELSE Check(d1)
  IN /\ outcome' = o
     /\ dirDur' = (IF refA THEN d0 ELSE d1)
     /\ phase' = "chk"
     /\ ev' = ev \cup (IF named /\ ~refA THEN {"adopt"} ELSE {}) \cup (IF o.ign THEN {"ignore"} ELSE {})
  /\ UNCHANGED <<ino, nIno, dirMem, pend, wm, wIno, hdrPend, hv, spSt, dPend, intended, acked, vt,
                 ltr, term, ntr, floor, ckpt, nseed, fault, crashes, nroll>>

\* After a refusal: stop. After an accepted recovery: the log is the recovered one, and
\* the writer opens E + 1 (3(c)): a segment there holding only its named header and
\* format sectors is opened as is; any other is replaced by 3(a) keeping its nonce (3(b));
\* otherwise E + 1 is created by 3(a). A lost format write under the replacement leaves
\* old content: the replaced file's own sectors (this nonce), or a freed file's sectors,
\* which are damage unless frames are not checked for the nonce (NoFrameNonce).
Resume == phase = "chk" /\
  IF outcome.st = "refused"
  THEN /\ phase' = "done"
       /\ UNCHANGED <<ino, nIno, dirDur, dirMem, pend, wm, wIno, hdrPend, hv, spSt, dPend,
                      intended, acked, vt, ltr, term, ntr, floor, ckpt, nseed, outcome, fault,
                      crashes, ev, nroll>>
  ELSE
  LET d == dirDur
      log == outcome.log
      E == Len(log)
      k == E + 1
      S == SegOfD(d, E)
      hd == IF k = floor THEN [prev |-> 0, pn |-> 0]
            ELSE [prev |-> Term(log[E]), pn |-> PnOf(d[S])]
      fresh == [first |-> k, prev |-> hd.prev, pn |-> hd.pn, recs |-> EmptyRecs]
      j == d[k]
      asIs == j # 0 /\ ino[j].first = k /\ \A x \in 1 .. MaxLsn : ino[j].recs[x] = EMPTY
      Freed == {q \in 1 .. nIno : q # j /\ ~\E m \in Nm : d[m] = q}
      Foreign(q) == [x \in 1 .. MaxLsn |->
                       IF x >= k /\ Valid(ino[q].recs[x])
                       THEN (IF MUT = "NoFrameNonce" THEN ino[q].recs[x] ELSE DMG) ELSE EMPTY]
      Views == {EmptyRecs} \cup
               (IF FaultOn /\ "format" \in FK /\ fault = "none"
                THEN {[x \in 1 .. MaxLsn |-> IF x >= k THEN ino[j].recs[x] ELSE EMPTY]}
                     \cup {Foreign(q) : q \in Freed}
                ELSE {})
  IN /\ intended' = log /\ phase' = "run" /\ spSt' = "none"
     /\ CASE asIs ->
             /\ wIno' = j
             /\ UNCHANGED <<ino, nIno, dirDur, fault>> /\ ev' = ev
          [] j # 0 ->
             \E v \in Views :
               /\ fault' = (IF v = EmptyRecs THEN fault ELSE "lostFormat")
               /\ ev' = ev \cup {"replace"} \cup (IF v = EmptyRecs THEN {} ELSE {"stale"})
               /\ IF MUT = "ReplaceNewNonce"
                  THEN /\ ino' = Append(ino, [fresh EXCEPT !.recs = v])
                       /\ nIno' = nIno + 1 /\ dirDur' = [d EXCEPT ![k] = nIno + 1]
                       /\ wIno' = nIno + 1
                  ELSE /\ ino' = [ino EXCEPT ![j] = [fresh EXCEPT !.recs = v]]
                       /\ wIno' = j /\ UNCHANGED <<nIno, dirDur>>
          [] OTHER ->
             /\ ino' = Append(ino, fresh) /\ nIno' = nIno + 1
             /\ Sync(<<[name |-> k, nonce |-> nIno + 1, mv |-> FALSE]>>)
             /\ wIno' = nIno + 1 /\ ev' = ev \cup {"reopenNew"}
     /\ dirMem' = dirDur'
     /\ UNCHANGED <<pend, wm, hdrPend, hv, dPend, acked, vt, ltr, term, ntr, floor, ckpt, nseed,
                    outcome, crashes, nroll>>

Next == AppData \/ DataLand \/ AppWm \/ OpenSeg \/ TakeRen \/ TakeHdr \/ HdrFirst \/ RenAfter
        \/ Trunc \/ Reseed \/ Crash \/ Recover \/ Resume

Spec == Init /\ [][Next]_vars

\* ------------------------------ properties --------------------------------
Accepted == phase = "chk" /\ outcome.st = "ok"
\* crashes alone never refuse
WmNoCrashRefusal == outcome.st = "refused" => fault # "none"
\* an accepted log holds no voided entry: it is a prefix of the node's current log
NoVoidRevival == Accepted =>
                   /\ Len(outcome.log) <= Len(intended)
                   /\ \A x \in 1 .. Len(outcome.log) : outcome.log[x] = intended[x]
\* every acknowledged entry of the current log survives
NoSilentLoss == Accepted => \A a \in acked : a[1] <= Len(outcome.log) /\ outcome.log[a[1]] = a[2]

Safety == WmNoCrashRefusal /\ NoVoidRevival /\ NoSilentLoss

\* Reachability probes (each expected to be violated, i.e. reached).
PEv(t)         == ~(Accepted /\ t \in ev)
PRoll2         == PEv("roll2")
PTrunc2        == PEv("trunc2")
PReopenNew     == PEv("reopenNew")
PReplace       == PEv("replace")
PSpare         == PEv("spare")
PAdopt         == PEv("adopt")
PIgnore        == PEv("ignore")
PStale         == PEv("stale")
PSeed          == PEv("seed")
PUnlinkRefuse  == ~(outcome.st = "refused" /\ fault = "lostUnlink")
PBindRefuse    == ~(outcome.st = "refused" /\ fault = "lostBind")
POkAfterDirFault == ~(Accepted /\ fault \in {"lostUnlink", "lostBind"})
PSecondRec     == ~(Accepted /\ crashes = 2)
==============================================================================
