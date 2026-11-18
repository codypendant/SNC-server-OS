------------------------------ MODULE Completion ------------------------------
(* Crash consistency of the truncation completion procedure of AFR-D-06       *)
(* (docs/AFR_001_D06_STORAGE_METADATA.md 4.1, 4.3). One WAL stream on one     *)
(* node. Preregistration, bounds and limits: docs/AFR_001_COMPLETION_CRASH.md. *)
(*                                                                            *)
(* File system model:                                                         *)
(*  - file contents (inodes) are durable when written (each write is          *)
(*    followed by fdatasync in the design);                                   *)
(*  - directory operations (create, unlink, rename) are pending until a       *)
(*    directory fsync. A crash keeps any subsequence of the pending           *)
(*    operations, applied in order; each rename is atomic.                    *)
(*  - the metadata record is two copies (A/B); a write goes to the copy that  *)
(*    is not current and may be torn by a crash, which leaves it invalid.     *)
(* Not modelled: torn WAL writes inside a block, lost acknowledged writes     *)
(* (D-02, R-23), checkpoints and reclaim, multiple streams, the rebuild's     *)
(* contents (step 2.5 is a no-op here), timing.                               *)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS MaxLsn,     \* highest LSN written
          Cap,        \* records per segment
          MaxTrunc,   \* truncations per run
          MaxCrash,   \* crashes per run
          MaxIno,     \* inodes per run
          \* Mutations (each TRUE breaks one step of the design; all FALSE = design)
          MUnlinkReopen, \* writer open unlinks and recreates the segment at its name (today's code)
          MSpareUnlink,  \* completion unlinks the spare unconditionally
          MNoNewSeg,     \* completion does not create segment L+1 before the clear
          MClearEarly,   \* the intent is cleared before the files are changed
          MNoVoid        \* the scan reads every record of a segment (no void rule)

SPARE == MaxLsn + 2
TMP   == MaxLsn + 3
SegNames == 1 .. MaxLsn + 1
Names == 1 .. MaxLsn + 3
Rec == [l : 1 .. MaxLsn, v : 0 .. MaxTrunc]
NoSlot == [st |-> "N", L |-> 0]

VARIABLES ino,     \* inode -> [hdr : 0..MaxLsn+1 (0 = no valid header), recs : Seq(Rec)]
          nIno,    \* inodes allocated
          dNames,  \* durable directory: name -> inode (0 = absent)
          pend,    \* pending directory operations
          meta,    \* [a, b] copies: [ok, g, slot]
          phase,   \* "up", "trunc", "rec", "down", "fatal"
          pc,      \* step inside "trunc"/"rec"
          tL,      \* L of the truncation in progress
          wIno,    \* inode the writer appends to (0 = closed)
          wEnd,    \* writer's end LSN
          logical, \* ghost: acknowledged, not truncated records
          nTrunc, nCrash

vars == <<ino, nIno, dNames, pend, meta, phase, pc, tL, wIno, wEnd, logical, nTrunc, nCrash>>

\* ---- Directory --------------------------------------------------------------
ApplyOp(n, op) ==
  CASE op.k = "create" -> [n EXCEPT ![op.to] = op.i]
    [] op.k = "unlink" -> [n EXCEPT ![op.to] = 0]
    [] op.k = "rename" -> [n EXCEPT ![op.to] = n[op.from], ![op.from] = 0]

RECURSIVE ApplySel(_, _, _, _)
ApplySel(n, ops, S, k) ==
  IF k > Len(ops) THEN n
  ELSE ApplySel(IF k \in S THEN ApplyOp(n, ops[k]) ELSE n, ops, S, k + 1)

Cur == ApplySel(dNames, pend, DOMAIN pend, 1)

Do(op) == pend' = Append(pend, op) /\ UNCHANGED dNames
Fsync == dNames' = Cur /\ pend' = <<>>
DoSync(op) == dNames' = ApplyOp(Cur, op) /\ pend' = <<>>

NewIno(h, r) == /\ nIno < MaxIno
                /\ nIno' = nIno + 1
                /\ ino' = [ino EXCEPT ![nIno + 1] = [hdr |-> h, recs |-> r]]

\* ---- Scan (storage.cpp scan_stream, with the void rule of 4.3) --------------
Segs(n) == {s \in SegNames : n[s] # 0}
NextSeg(n, s) == LET later == {x \in Segs(n) : x > s}
                 IN IF later = {} THEN 0 ELSE CHOOSE x \in later : \A y \in later : x <= y

\* Records of segment s that the scan may read: below the next segment's first LSN.
Readable(n, s) ==
  LET r == ino[n[s]].recs
      nx == NextSeg(n, s)
      keep(i) == MNoVoid \/ nx = 0 \/ r[i].l < nx
      RECURSIVE pre(_)
      pre(i) == IF i > Len(r) \/ ~keep(i) THEN i - 1 ELSE pre(i + 1)
  IN SubSeq(r, 1, pre(1))

RECURSIVE ScanFrom(_, _, _)
\* Returns [f |-> TRUE] for a fatal stop, else [f |-> FALSE, a |-> records read].
Bad == [f |-> TRUE, a |-> <<>>]
ScanFrom(n, s, acc) ==
  IF s = 0 THEN [f |-> FALSE, a |-> acc]
  ELSE LET exp == Len(acc) + 1
           last == NextSeg(n, s) = 0
           h == ino[n[s]].hdr
       IN IF h # s
            THEN (IF last /\ ino[n[s]].recs = <<>> THEN [f |-> FALSE, a |-> acc] ELSE Bad)  \* bad header
          ELSE IF s # exp THEN Bad                                                         \* gap before segment
          ELSE LET r == Readable(n, s)
                   RECURSIVE take(_, _)
                   take(i, a) == IF i > Len(r) THEN a
                                 ELSE IF r[i].l = Len(a) + 1 THEN take(i + 1, Append(a, r[i]))
                                 ELSE a                                      \* hole: rest never acknowledged
               IN ScanFrom(n, NextSeg(n, s), take(1, acc))

Scan(n) == LET first == IF Segs(n) = {} THEN 0
                        ELSE CHOOSE x \in Segs(n) : \A y \in Segs(n) : x <= y
           IN ScanFrom(n, first, <<>>)

\* ---- Metadata record --------------------------------------------------------
Best == IF meta.a.ok /\ (~meta.b.ok \/ meta.a.g > meta.b.g) THEN meta.a ELSE meta.b
Other == IF Best = meta.a THEN "b" ELSE "a"
MetaWrite(s) == meta' = [meta EXCEPT ![Other] = [ok |-> TRUE, g |-> Best.g + 1, slot |-> s]]

\* ---- Initial state: one empty segment 1, both record copies written ---------
Init == /\ ino = [i \in 1 .. MaxIno |-> IF i = 1 THEN [hdr |-> 1, recs |-> <<>>]
                                       ELSE [hdr |-> 0, recs |-> <<>>]]
        /\ nIno = 1
        /\ dNames = [n \in Names |-> IF n = 1 THEN 1 ELSE 0]
        /\ pend = <<>>
        /\ meta = [a |-> [ok |-> TRUE, g |-> 2, slot |-> NoSlot],
                   b |-> [ok |-> TRUE, g |-> 1, slot |-> NoSlot]]
        /\ phase = "up" /\ pc = 0 /\ tL = 0
        /\ wIno = 1 /\ wEnd = 0
        /\ logical = <<>> /\ nTrunc = 0 /\ nCrash = 0

Ghost == UNCHANGED <<logical, nTrunc, nCrash>>

\* ---- Writer (phase "up") -----------------------------------------------------
Append1 == /\ phase = "up" /\ wEnd < MaxLsn /\ Len(ino[wIno].recs) < Cap
           /\ LET r == [l |-> wEnd + 1, v |-> nTrunc]
              IN /\ ino' = [ino EXCEPT ![wIno].recs = Append(@, r)]
                 /\ logical' = Append(logical, r)
           /\ wEnd' = wEnd + 1
           /\ UNCHANGED <<nIno, dNames, pend, meta, phase, pc, tL, wIno, nTrunc, nCrash>>

\* Rollover onto a prepared spare: header written with the first record, rename not synced.
TakeSpare == /\ phase = "up" /\ wEnd < MaxLsn /\ Len(ino[wIno].recs) = Cap
             /\ Cur[SPARE] # 0 /\ ino[Cur[SPARE]].hdr = 0
             /\ LET sp == Cur[SPARE]
                    r == [l |-> wEnd + 1, v |-> nTrunc]
                IN /\ ino' = [ino EXCEPT ![sp] = [hdr |-> wEnd + 1, recs |-> <<r>>]]
                   /\ logical' = Append(logical, r)
                   /\ wIno' = sp
                   /\ Do([k |-> "rename", from |-> SPARE, to |-> wEnd + 1])
             /\ wEnd' = wEnd + 1
             /\ UNCHANGED <<nIno, dNames, meta, phase, pc, tL, nTrunc, nCrash>>

\* Rollover without a spare: create the segment, header, fsync, directory fsync (open_segment).
NewSegment == /\ phase = "up" /\ wEnd < MaxLsn /\ Len(ino[wIno].recs) = Cap
              /\ Cur[wEnd + 1] = 0
              /\ NewIno(wEnd + 1, <<>>)
              /\ DoSync([k |-> "create", to |-> wEnd + 1, i |-> nIno + 1])
              /\ wIno' = nIno + 1
              /\ UNCHANGED <<meta, phase, pc, tL, wEnd>> /\ Ghost

Prepare == /\ phase = "up" /\ Cur[SPARE] = 0
           /\ NewIno(0, <<>>)
           /\ DoSync([k |-> "create", to |-> SPARE, i |-> nIno + 1])
           /\ UNCHANGED <<meta, phase, pc, tL, wIno, wEnd>> /\ Ghost

\* ---- Truncation: intent (4.3 step 1) ----------------------------------------
Intent == /\ phase = "up" /\ nTrunc < MaxTrunc
          /\ \E L \in 0 .. wEnd - 1 :
               /\ MetaWrite([st |-> "P", L |-> L])
               /\ logical' = SubSeq(logical, 1, L)
               /\ tL' = L
               /\ wEnd' = L
          /\ nTrunc' = nTrunc + 1
          /\ phase' = "trunc" /\ pc' = 1 /\ wIno' = 0
          /\ UNCHANGED <<ino, nIno, dNames, pend, nCrash>>

\* ---- Completion steps, shared by "trunc" and "rec" --------------------------
\* pc: 1 spare, 2 delete above L, 3 fsync, 4 create tmp, 5 rename tmp, 6 fsync,
\*     7 clear, 8 reopen (rec: then adopt spare and scan).
InComp == phase \in {"trunc", "rec"} /\ pc \in 1 .. 7
Next1(p) == pc' = p

ClearStep == IF MClearEarly THEN 1 ELSE 7
AfterClear == IF phase = "rec" THEN 11 ELSE 8   \* recovery goes on to spare adoption and the scan

CSpare == /\ InComp /\ pc = 1 /\ ~(MClearEarly /\ Best.slot.st = "P")
          /\ LET sp == Cur[SPARE] IN
             IF sp = 0 THEN UNCHANGED <<dNames, pend>>
             ELSE IF ~MSpareUnlink /\ ino[sp].hdr # 0 /\ ino[sp].hdr <= tL
                    THEN DoSync([k |-> "rename", from |-> SPARE, to |-> ino[sp].hdr])  \* adopt
                    ELSE Do([k |-> "unlink", to |-> SPARE])
          /\ Next1(2)
          /\ UNCHANGED <<ino, nIno, meta, phase, tL, wIno, wEnd>> /\ Ghost

CDelete == /\ InComp /\ pc = 2
           /\ LET above == {s \in Segs(Cur) : s > tL} IN
              IF above = {} THEN Next1(3) /\ UNCHANGED <<dNames, pend>>
              ELSE LET top == CHOOSE s \in above : \A y \in above : y <= s
                   IN Do([k |-> "unlink", to |-> top]) /\ UNCHANGED pc
           /\ UNCHANGED <<ino, nIno, meta, phase, tL, wIno, wEnd>> /\ Ghost

CFsync1 == /\ InComp /\ pc = 3 /\ Fsync
           /\ Next1(IF MNoNewSeg THEN 7 ELSE 4)
           /\ UNCHANGED <<ino, nIno, meta, phase, tL, wIno, wEnd>> /\ Ghost

CTmp == /\ InComp /\ pc = 4
        /\ NewIno(tL + 1, <<>>)
        /\ Do([k |-> "create", to |-> TMP, i |-> nIno + 1])
        /\ Next1(5)
        /\ UNCHANGED <<meta, phase, tL, wIno, wEnd>> /\ Ghost

CRename == /\ InComp /\ pc = 5
           /\ Do([k |-> "rename", from |-> TMP, to |-> tL + 1])
           /\ Next1(6)
           /\ UNCHANGED <<ino, nIno, meta, phase, tL, wIno, wEnd>> /\ Ghost

CFsync2 == /\ InComp /\ pc = 6 /\ Fsync /\ Next1(IF MClearEarly THEN AfterClear ELSE 7)
           /\ UNCHANGED <<ino, nIno, meta, phase, tL, wIno, wEnd>> /\ Ghost

\* The clear: compare-and-write on the slot being PENDING(L). Under MClearEarly it runs first.
CClear == /\ phase \in {"trunc", "rec"} /\ Best.slot = [st |-> "P", L |-> tL]
          /\ pc = ClearStep
          /\ MetaWrite(NoSlot)
          /\ Next1(IF MClearEarly THEN 1 ELSE AfterClear)
          /\ UNCHANGED <<ino, nIno, dNames, pend, phase, tL, wIno, wEnd>> /\ Ghost

\* Torn write of the record: the target copy is left invalid and the node crashes.
TornCrash == /\ phase \in {"up", "trunc", "rec"} /\ nCrash < MaxCrash
             /\ \/ phase = "up" /\ nTrunc < MaxTrunc
                \/ phase \in {"trunc", "rec"} /\ pc = ClearStep /\ Best.slot.st = "P"
             /\ meta' = [meta EXCEPT ![Other] = [ok |-> FALSE, g |-> 0, slot |-> NoSlot]]
             /\ \E S \in SUBSET DOMAIN pend : dNames' = ApplySel(dNames, pend, S, 1)
             /\ pend' = <<>>
             /\ phase' = "down" /\ pc' = 0 /\ wIno' = 0 /\ nCrash' = nCrash + 1
             /\ UNCHANGED <<ino, nIno, tL, wEnd, logical, nTrunc>>

\* ---- Writer open (4.3 step 3; storage.cpp:521-527 under MUnlinkReopen) ------
\* pc 8: open at e+1 where e is the end (wEnd). pc 9: MUnlinkReopen's create; pc 10: its fsync.
Reopen == /\ phase \in {"trunc", "rec"} /\ pc = 8
          /\ LET n == wEnd + 1 IN
             IF Cur[n] # 0 /\ ~MUnlinkReopen
               THEN /\ ino' = [ino EXCEPT ![Cur[n]] = [hdr |-> n, recs |-> <<>>]]  \* truncate to header
                    /\ wIno' = Cur[n] /\ phase' = "up" /\ pc' = 0
                    /\ UNCHANGED <<nIno, dNames, pend>>
             ELSE IF Cur[n] # 0
               THEN Do([k |-> "unlink", to |-> n]) /\ pc' = 9
                    /\ UNCHANGED <<ino, nIno, wIno, phase>>
             ELSE /\ NewIno(n, <<>>) /\ DoSync([k |-> "create", to |-> n, i |-> nIno + 1])
                  /\ wIno' = nIno + 1 /\ phase' = "up" /\ pc' = 0
          /\ UNCHANGED <<meta, tL, wEnd>> /\ Ghost

ReopenCreate == /\ phase \in {"trunc", "rec"} /\ pc = 9
                /\ NewIno(wEnd + 1, <<>>)
                /\ Do([k |-> "create", to |-> wEnd + 1, i |-> nIno + 1])
                /\ pc' = 10 /\ wIno' = nIno + 1
                /\ UNCHANGED <<meta, phase, tL, wEnd>> /\ Ghost

ReopenSync == /\ phase \in {"trunc", "rec"} /\ pc = 10 /\ Fsync
              /\ phase' = "up" /\ pc' = 0
              /\ UNCHANGED <<ino, nIno, meta, tL, wIno, wEnd>> /\ Ghost

\* ---- Crash and recovery -------------------------------------------------------
Crash == /\ phase \in {"up", "trunc", "rec"} /\ nCrash < MaxCrash
         /\ \E S \in SUBSET DOMAIN pend : dNames' = ApplySel(dNames, pend, S, 1)
         /\ pend' = <<>>
         /\ phase' = "down" /\ pc' = 0 /\ wIno' = 0
         /\ nCrash' = nCrash + 1
         /\ UNCHANGED <<ino, nIno, meta, tL, wEnd, logical, nTrunc>>

\* Recovery: read the record; a pending intent is completed first.
RecStart == /\ phase = "down"
            /\ IF Best.slot.st = "P"
                 THEN tL' = Best.slot.L /\ pc' = 1
                 ELSE tL' = 0 /\ pc' = 11
            /\ phase' = "rec"
            /\ UNCHANGED <<ino, nIno, dNames, pend, meta, wIno, wEnd>> /\ Ghost

\* pc 11: adopt a spare (storage.cpp:797-812); pc 12: scan, then open the writer at pc 8.
RecSpare == /\ phase = "rec" /\ pc = 11
            /\ LET sp == Cur[SPARE] IN
               IF sp = 0 THEN UNCHANGED <<dNames, pend>>
               ELSE IF ino[sp].hdr # 0
                      THEN DoSync([k |-> "rename", from |-> SPARE, to |-> ino[sp].hdr])
                      ELSE Do([k |-> "unlink", to |-> SPARE])
            /\ pc' = 12
            /\ UNCHANGED <<ino, nIno, meta, phase, tL, wIno, wEnd>> /\ Ghost

RecScan == /\ phase = "rec" /\ pc = 12
           /\ LET s == Scan(Cur) IN
              IF s.f THEN phase' = "fatal" /\ UNCHANGED <<pc, wEnd>>
              ELSE wEnd' = Len(s.a) /\ pc' = 8 /\ UNCHANGED phase
           /\ UNCHANGED <<ino, nIno, dNames, pend, meta, tL, wIno>> /\ Ghost

Next == \/ Append1 \/ TakeSpare \/ NewSegment \/ Prepare \/ Intent
        \/ CSpare \/ CDelete \/ CFsync1 \/ CTmp \/ CRename \/ CFsync2 \/ CClear
        \/ Reopen \/ ReopenCreate \/ ReopenSync
        \/ Crash \/ TornCrash \/ RecStart \/ RecSpare \/ RecScan

Spec == Init /\ [][Next]_vars

\* ---- Properties -----------------------------------------------------------------
\* Every state in which the node serves: the log a recovery would read is exactly
\* the acknowledged, non-truncated log. No void record is read (no revival) and no
\* acknowledged record is lost.
ServedLogExact == phase = "up" => Scan(Cur) = [f |-> FALSE, a |-> logical]
NoFatal == phase # "fatal"
Safety == ServedLogExact /\ NoFatal

\* Reachability probes (each is expected to be VIOLATED, which shows the case is reached).
PRecComplete == ~(phase = "rec" /\ pc = 2)          \* recovery completes a pending intent
PTwoTrunc    == ~(phase = "up" /\ nTrunc = 2)       \* serving after two truncations
PInoBound    == nIno < MaxIno                       \* expected HELD: the inode bound is never hit
=============================================================================
