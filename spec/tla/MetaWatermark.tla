---------------------------- MODULE MetaWatermark ----------------------------
(* Crash consistency of the D-02 crash design under crashes plus at most one  *)
(* media fault (docs/AFR_001_D02_CRASH_DESIGN.md sections 2-5). Two parts,   *)
(* selected by PART; the other part's variables stay at their initial value. *)
(* Preregistration, bounds and limits: docs/AFR_001_D02_MODEL.md.            *)
(*                                                                            *)
(* PART "rec": the node record. Four slots a0 a1 b0 b1; generation g goes to *)
(*  slot g%2 of both files. A write is pending until it lands; a crash lands  *)
(*  any subset of pending writes, tears any subset, drops the rest. The node  *)
(*  acts on a value only after all its writes landed (fdatasync returned).    *)
(*  Recovery: read (tie => refuse; no valid slot => init incomplete), rule 4  *)
(*  (duplicate a single-copy generation into its own pair), boot write, act.  *)
(*  Faults (one per run): a landing write is lost, torn or misdirected to     *)
(*  another slot; or any one slot is zeroed at any time (a foreign write;     *)
(*  slot spacing makes it hit one slot).                                      *)
(*                                                                            *)
(* PART "wm": one WAL stream, a truncation to L, the watermark. The record is *)
(*  atomic here (PART "rec" checks it). Inodes are durable when written;      *)
(*  directory operations are pending until a directory fsync, and a crash     *)
(*  keeps any ordered subsequence of them. Faults (one per run): a lost data  *)
(*  write, a lost watermark write, or one directory operation lost after its  *)
(*  fsync. Recovery is terminal: completion reruns if PENDING (crashable),    *)
(*  then the scan and the checks of sections 3-5 decide.                      *)
(* Revision 3: every watermark after the truncation carries the L + 1 binding *)
(*  (OD-2); constant Layout adds a void tail of two records in segment 1.     *)
(* Not modelled: the floor and reclaim (O-FLOOR), torn or misdirected WAL     *)
(* writes (X-2 cases), several streams, the wire, process-kill I/O (V8).      *)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS PART,      \* "rec" or "wm"
          MUT,       \* "none" or one mutation name (see MutOk)
          MaxGen,    \* rec: highest generation written
          MaxCrash,  \* crashes per run
          FaultOn    \* TRUE: one media fault may happen

\* ============================== PART rec ==================================
Slots == {"a0", "a1", "b0", "b1"}
Order == <<"a0", "a1", "b0", "b1">>          \* device order if files are adjacent
Bad == <<0, 0>>                              \* invalid (torn, zeroed, never written)
Pair(g) == IF g % 2 = 0 THEN {"a0", "b0"} ELSE {"a1", "b1"}
SlotOf(f, g) == IF g % 2 = 0 THEN (IF f = "a" THEN "a0" ELSE "b0")
                             ELSE (IF f = "a" THEN "a1" ELSE "b1")

VARIABLES slot,    \* slot -> <<g, v>>  (v identifies the bytes)
          pw,      \* pending writes: set of [s, val]
          queue,   \* K5 only: second write issued after the first lands
          stage,   \* what completes when pw = {}: "init1" "init2" "rt" "r4" "boot"
          rpc,     \* "write", "run", "rec", "stop"
          target,  \* value the node acts on when the stage completes
          acted,   \* ghost: newest value acted on
          nv,      \* next byte-version id
          chosen,  \* ghost: value recovery read last (for the invariant)
          refusedR \* recovery refused
\* ============================== PART wm ===================================
CONSTANTS L, MaxLsn,
          Pipe,      \* wm: TRUE lets data(x + 1) be in flight with data(x)
          Layout     \* wm: "A" segment 1 = LSN 1..2, segment 3 = LSN 3; "B" segment 1 = LSN 1..3
VARIABLES ino,     \* nonce -> [first, prev, recs : [1..MaxLsn -> entry or 0]]
          nIno,
          dirDur,  \* durable bindings: name -> nonce (0 = none)
          dirMem,  \* in-memory view
          pend,    \* pending directory operations (sequence)
          wm,      \* durable watermark [lsn, e, nonce]; nonce # 0 binds name L + 1 (3(e), OD-2)
          wmCo,    \* coalesced watermark not yet submitted (or NoWm)
          recSt,   \* record slot: "NONE" or "PENDING"
          bpc,     \* program counter of part wm
          wIno,    \* writer's inode
          nextX,   \* next LSN to append
          nextW,   \* next LSN to watermark
          intended,\* ghost: the node's current log (sequence of entries)
          acked,   \* ghost: acknowledged <<x, e>> that must survive
          dPend,   \* data writes in flight: set of [x, e]
          outcome  \* [st: "none" "refused" "ok", log: the recovered log]
\* ============================== shared ====================================
VARIABLES fault,   \* "none" or the kind used
          crashes

recVars == <<slot, pw, queue, stage, rpc, target, acted, nv, chosen, refusedR>>
wmVars  == <<ino, nIno, dirDur, dirMem, pend, wm, wmCo, recSt, bpc, wIno, nextX, nextW,
             nextW, intended, acked, dPend, outcome>>
vars == <<recVars, wmVars, fault, crashes>>

MutOk == MUT \in {"none", "NoRule4", "BootFirst", "Adjacent", "K5", "AB",
                  "LowerAfterClear", "NoDrain", "NoNonceCheck", "AckBeforeWm", "BindOnce"}

\* ---------------------------- rec: helpers --------------------------------
Valid == {s \in Slots : slot[s][1] > 0}
MaxVal == IF Valid = {} THEN Bad
          ELSE CHOOSE v \in {slot[s] : s \in Valid} :
                 \A s \in Valid : slot[s][1] <= v[1]
Tie == \E s1, s2 \in Valid : slot[s1][1] = slot[s2][1] /\ slot[s1] # slot[s2]

\* Targets of a write of value val under the design or a mutation.
Targets(val) == CASE MUT = "AB" -> {SlotOf("a", val[1])}
                  [] MUT = "K5" -> {"a0"}
                  [] OTHER      -> Pair(val[1])

Issue(val, st) ==
  /\ pw' = {[s |-> t, val |-> val] : t \in Targets(val)}
  /\ queue' = (IF MUT = "K5" THEN {[s |-> "b0", val |-> val]} ELSE {})
  /\ stage' = st /\ target' = val /\ rpc' = "write"

RecInit ==
  /\ slot = [s \in Slots |-> Bad] /\ acted = Bad /\ chosen = Bad
  /\ queue = (IF PART = "rec" /\ MUT = "K5" THEN {[s |-> "b0", val |-> <<1, 1>>]} ELSE {})
  /\ refusedR = FALSE
  /\ IF PART = "rec"
       THEN /\ pw = {[s |-> t, val |-> <<1, 1>>] : t \in Targets(<<1, 1>>)}
            /\ stage = "init1" /\ rpc = "write" /\ target = <<1, 1>> /\ nv = 2
       ELSE /\ pw = {} /\ stage = "none" /\ rpc = "stop" /\ target = Bad /\ nv = 1

\* A pending write lands normally.
Land == \E w \in pw :
  /\ slot' = [slot EXCEPT ![w.s] = w.val]
  /\ pw' = pw \ {w}
  /\ UNCHANGED <<queue, stage, rpc, target, acted, nv, chosen, refusedR, fault>>

\* One fault on a landing write: lost, torn, or misdirected to another slot.
LandFault == fault = "none" /\ FaultOn /\ \E w \in pw :
  /\ pw' = pw \ {w}
  /\ \/ slot' = slot /\ fault' = "lost"
     \/ slot' = [slot EXCEPT ![w.s] = Bad] /\ fault' = "torn"
     \/ \E s2 \in Slots \ {w.s} : slot' = [slot EXCEPT ![s2] = w.val] /\ fault' = "misdirected"
  /\ UNCHANGED <<queue, stage, rpc, target, acted, nv, chosen, refusedR>>

\* One foreign write zeroes slots: one slot (spacing), or three in a row (Adjacent).
Foreign == fault = "none" /\ FaultOn /\ rpc # "stop" /\
  \/ /\ MUT # "Adjacent"
     /\ \E s \in Slots : slot' = [slot EXCEPT ![s] = Bad]
  \/ /\ MUT = "Adjacent"
     /\ \E i \in 1..2 : slot' = [t \in Slots |->
           IF \E j \in i..i+2 : Order[j] = t THEN Bad ELSE slot[t]]
  /\ fault' = "foreign"
  /\ UNCHANGED <<pw, queue, stage, rpc, target, acted, nv, chosen, refusedR>>

\* All writes of the stage returned: K5 issues its second write, else act.
Synced == rpc = "write" /\ pw = {} /\
  IF queue # {}
  THEN /\ pw' = queue /\ queue' = {}
       /\ UNCHANGED <<slot, stage, rpc, target, acted, nv, chosen, refusedR, fault>>
  ELSE /\ CASE stage = "init1" ->
                 /\ pw' = {[s |-> t, val |-> <<2, nv>>] : t \in Targets(<<2, nv>>)}
                 /\ queue' = (IF MUT = "K5" THEN {[s |-> "b0", val |-> <<2, nv>>]} ELSE {})
                 /\ stage' = "init2" /\ target' = <<2, nv>> /\ nv' = nv + 1
                 /\ UNCHANGED <<rpc, acted>>
            [] stage = "bootFirst" ->  \* BootFirst: rule-4 copies of the chosen value next
                 /\ pw' = {[s |-> t, val |-> chosen] : t \in
                           {SlotOf(f, chosen[1]) : f \in {"a", "b"}} \ {s \in Slots : slot[s] = chosen}}
                 /\ queue' = {} /\ stage' = "boot"
                 /\ UNCHANGED <<rpc, acted, target, nv>>
            [] stage = "r4" ->  \* rule 4 done: boot write next
                 /\ LET c == target IN
                    IF c[1] + 1 > MaxGen
                    THEN /\ rpc' = "stop" /\ acted' = c /\ queue' = {}
                         /\ UNCHANGED <<pw, stage, target, nv>>
                    ELSE /\ pw' = {[s |-> t, val |-> <<c[1] + 1, nv>>] : t \in Targets(<<c[1] + 1, nv>>)}
                         /\ queue' = (IF MUT = "K5" THEN {[s |-> "b0", val |-> <<c[1] + 1, nv>>]} ELSE {})
                         /\ stage' = "boot" /\ target' = <<c[1] + 1, nv>> /\ nv' = nv + 1
                         /\ UNCHANGED <<rpc, acted>>
            [] OTHER -> \* init2, rt, boot: act on the target
                 /\ acted' = target /\ rpc' = "run" /\ queue' = {}
                 /\ UNCHANGED <<pw, stage, target, nv>>
       /\ UNCHANGED <<slot, chosen, refusedR, fault>>

\* Runtime record write (any writer of D-06 4.1).
RunWrite == rpc = "run" /\ target[1] < MaxGen /\
  LET val == <<target[1] + 1, nv>> IN
  /\ Issue(val, "rt") /\ nv' = nv + 1
  /\ UNCHANGED <<slot, acted, chosen, refusedR, fault>>

RCrash(crashOk) == crashOk /\ rpc # "stop" /\
  \E landed, torn \in SUBSET pw :
    /\ landed \cap torn = {}
    /\ slot' = [s \in Slots |->
         IF \E w \in torn : w.s = s THEN Bad
         ELSE IF \E w \in landed : w.s = s THEN (CHOOSE w \in landed : w.s = s).val
         ELSE slot[s]]
    /\ pw' = {} /\ queue' = {} /\ rpc' = "rec"
    /\ UNCHANGED <<stage, target, acted, nv, chosen, refusedR, fault>>

\* Recovery: read, then init / refuse / rule 4 / boot write.
Recover == rpc = "rec" /\
  LET c == MaxVal
      missing == {SlotOf(f, c[1]) : f \in {"a", "b"}} \ {s \in Slots : slot[s] = c}
  IN
  /\ chosen' = c
  /\ CASE Tie -> /\ refusedR' = TRUE /\ rpc' = "stop"
                 /\ UNCHANGED <<pw, queue, stage, target, nv>>
       [] Valid = {} ->  \* init incomplete: init again
                 /\ pw' = {[s |-> t, val |-> <<1, nv>>] : t \in Targets(<<1, nv>>)}
                 /\ queue' = (IF MUT = "K5" THEN {[s |-> "b0", val |-> <<1, nv>>]} ELSE {})
                 /\ stage' = "init1" /\ target' = <<1, nv>> /\ nv' = nv + 1 /\ rpc' = "write"
                 /\ UNCHANGED refusedR
       [] MUT = "BootFirst" /\ missing # {} /\ c[1] + 1 <= MaxGen ->
                 /\ pw' = {[s |-> t, val |-> <<c[1] + 1, nv>>] : t \in Targets(<<c[1] + 1, nv>>)}
                 /\ queue' = {} /\ stage' = "bootFirst" /\ target' = <<c[1] + 1, nv>>
                 /\ nv' = nv + 1 /\ rpc' = "write"
                 /\ UNCHANGED refusedR
       [] MUT \notin {"NoRule4", "K5", "AB"} /\ missing # {} ->  \* rule 4
                 /\ pw' = {[s |-> t, val |-> c] : t \in missing}
                 /\ queue' = {} /\ stage' = "r4" /\ target' = c /\ rpc' = "write"
                 /\ UNCHANGED <<nv, refusedR>>
       [] OTHER -> \* straight to the boot write, through stage r4 with nothing pending
                 /\ pw' = {} /\ queue' = {} /\ stage' = "r4" /\ target' = c /\ rpc' = "write"
                 /\ UNCHANGED <<nv, refusedR>>
  /\ UNCHANGED <<slot, acted, fault>>

\* ---------------------------- wm: helpers ---------------------------------
Old(x) == <<1, x>>                 \* entry = <<term, lsn>>; content is fixed by G-15
New(x) == <<2, x>>
NoWm == [lsn |-> 0, e |-> <<0, 0>>, nonce |-> 0]
Names == 1 .. MaxLsn
Term(e) == e[1]

ApplyOp(d, op) == [d EXCEPT ![op.name] = op.nonce]   \* nonce 0 = unlink
ApplySeq(d, s) == LET F[i \in 0..Len(s)] == IF i = 0 THEN d ELSE ApplyOp(F[i-1], s[i])
                  IN F[Len(s)]
Present(d) == {n \in Names : d[n] # 0}
\* Segment holding LSN x in directory view d (largest name <= x)
SegOf(d, x) == IF \E n \in Present(d) : n <= x
               THEN CHOOSE n \in Present(d) : n <= x /\ \A m \in Present(d) : m <= x => m <= n
               ELSE 0

\* Recovery scan with the void rule by name, FL-floor (first segment is 1, prev 0)
\* and FL-chain; returns [ok, log].
Scan(d) ==
  LET P == Present(d)
      Next(n) == IF \E m \in P : m > n THEN CHOOSE m \in P : m > n /\ \A k \in P : k > n => k >= m
                 ELSE 0
      Recs(n) == ino[d[n]].recs
      \* contiguous run of records from n, up to (but excluding) limit
      RunEnd(n, limit) == LET R == {x \in n..MaxLsn : x < limit /\ \A y \in n..x : Recs(n)[y] # <<0, 0>>}
                          IN IF R = {} THEN n - 1 ELSE CHOOSE x \in R : \A y \in R : y <= x
      Last == CHOOSE n \in P : \A m \in P : m <= n
      E == RunEnd(Last, MaxLsn + 1)
      GapFree == \A n \in P : Next(n) # 0 => RunEnd(n, Next(n)) = Next(n) - 1
      LogE == [x \in 1..E |-> Recs(SegOf(d, x))[x]]
      \* design 4, "above the watermark": only format sectors may be discarded, so a
      \* valid record after the tail's first gap refuses
      Stray == \E y \in (E + 1) .. MaxLsn : Recs(Last)[y] # <<0, 0>>
      Chain == \A n \in P : n > 1 => ino[d[n]].prev = Term(Recs(SegOf(d, n - 1))[n - 1])
  IN IF P = {} \/ 1 \notin P \/ ino[d[1]].prev # 0 THEN [ok |-> FALSE, log |-> <<>>]
     ELSE IF ~GapFree THEN [ok |-> FALSE, log |-> <<>>]
     ELSE IF ~Chain \/ Stray THEN [ok |-> FALSE, log |-> <<>>]
     ELSE [ok |-> TRUE, log |-> LogE]

Check(d) ==
  LET s == Scan(d)
      E == Len(s.log)
      Ref == [st |-> "refused", log |-> <<>>]
  IN IF ~s.ok THEN Ref
     ELSE IF wm.lsn > E THEN Ref
     ELSE IF wm.lsn >= 1 /\ s.log[wm.lsn] # wm.e THEN Ref
     ELSE IF MUT # "NoNonceCheck" /\ wm.nonce # 0 /\ d[L + 1] # wm.nonce THEN Ref
     ELSE [st |-> "ok", log |-> s.log]

EmptyRecs == [x \in 1..MaxLsn |-> <<0, 0>>]

WmInit ==
  /\ IF PART = "wm"
       THEN \* old log: segment 1 holds LSN 1..2, segment 3 holds LSN 3; acknowledged
            \* through 2; the watermark for 3 is coalesced, not yet submitted.
            \* Layout "B": all three in segment 1, so a truncation leaves a void tail of
            \* two records in it at L = 1 (revision 2 review, finding 1).
            /\ IF Layout = "A"
               THEN /\ ino = [n \in 1..2 |-> IF n = 1
                         THEN [first |-> 1, prev |-> 0,
                               recs |-> [x \in 1..MaxLsn |-> IF x <= 2 THEN Old(x) ELSE <<0, 0>>]]
                         ELSE [first |-> 3, prev |-> 1,
                               recs |-> [x \in 1..MaxLsn |-> IF x = 3 THEN Old(x) ELSE <<0, 0>>]]]
                    /\ nIno = 2
                    /\ dirDur = [n \in Names |-> CASE n = 1 -> 1 [] n = 3 -> 2 [] OTHER -> 0]
               ELSE /\ ino = <<[first |-> 1, prev |-> 0,
                                 recs |-> [x \in 1..MaxLsn |-> IF x <= 3 THEN Old(x) ELSE <<0, 0>>]]>>
                    /\ nIno = 1
                    /\ dirDur = [n \in Names |-> IF n = 1 THEN 1 ELSE 0]
            /\ dirMem = dirDur
            /\ wm = [lsn |-> 2, e |-> Old(2), nonce |-> 0]
            /\ wmCo = [lsn |-> 3, e |-> Old(3), nonce |-> 0]
            /\ bpc = "intent" /\ intended = <<Old(1), Old(2), Old(3)>>
            /\ acked = {<<1, Old(1)>>, <<2, Old(2)>>}
       ELSE /\ ino = <<>> /\ nIno = 0 /\ dirDur = <<>> /\ dirMem = <<>> /\ wm = NoWm
            /\ wmCo = NoWm /\ bpc = "stop" /\ intended = <<>> /\ acked = {}
  /\ pend = <<>> /\ recSt = "NONE" /\ wIno = 0 /\ nextX = L + 1 /\ nextW = L + 1
  /\ dPend = {} /\ outcome = [st |-> "none", log |-> <<>>]

\* What must survive: every end reported in the current history.
Required == acked

\* The coalesced watermark is submitted and lands (it carries the newest generation).
WmCoLand == wmCo # NoWm /\ bpc \notin {"rec", "done", "stop"} /\
  /\ \/ wm' = wmCo /\ fault' = fault
     \/ FaultOn /\ fault = "none" /\ wm' = wm /\ fault' = "lostWm"
  /\ wmCo' = NoWm
  \* its end is reported only before the intent; after it, LSN 3 is above L (void)
  /\ acked' = (IF bpc = "intent" THEN acked \cup {<<wmCo.lsn, wmCo.e>>} ELSE acked)
  /\ UNCHANGED <<ino, nIno, dirDur, dirMem, pend, recSt, bpc, wIno, nextX, nextW, intended,
                 dPend, outcome>>

DirSync ==
  /\ \/ dirDur' = ApplySeq(dirDur, pend) /\ fault' = fault
     \/ /\ FaultOn /\ fault = "none" /\ Len(pend) > 0
        /\ \E i \in 1..Len(pend) :
             /\ dirDur' = ApplySeq(dirDur, SubSeq(pend, 1, i - 1) \o SubSeq(pend, i + 1, Len(pend)))
             /\ fault' = (IF pend[i].nonce = 0 THEN "lostUnlink" ELSE "lostBind")
  /\ pend' = <<>>

\* Steps of the truncation and the new appends.
Intent == bpc = "intent" /\ recSt' = "PENDING" /\ bpc' = "c1"
          /\ intended' = SubSeq(intended, 1, L)
          \* entries above L were never HA_DURABLE (V4): their local ends are void
          /\ acked' = {a \in acked : a[1] <= L}
          /\ UNCHANGED <<ino, nIno, dirDur, dirMem, pend, wm, wmCo, wIno, nextX, nextW,
                         dPend, outcome, fault>>

\* 2.1: wait for the in-flight watermark (WmCoLand), or drop it while unsubmitted.
C1 == bpc = "c1" /\ bpc' = "c3"
      /\ \/ MUT = "NoDrain" /\ wmCo' = wmCo
         \/ MUT # "NoDrain" /\ wmCo' = NoWm
      /\ UNCHANGED <<ino, nIno, dirDur, dirMem, pend, wm, recSt, wIno, nextX, nextW, intended,
                     acked, dPend, outcome, fault>>

C3 == bpc = "c3" /\
  LET del == {n \in Present(dirMem) : n > L}
      ops == [i \in 1..Cardinality(del) |->
                [name |-> CHOOSE n \in del : Cardinality({m \in del : m > n}) = i - 1, nonce |-> 0]]
  IN /\ pend' = pend \o ops
     /\ dirMem' = [n \in Names |-> IF n \in del THEN 0 ELSE dirMem[n]]
     /\ bpc' = "c3s"
     /\ UNCHANGED <<ino, nIno, dirDur, wm, wmCo, recSt, wIno, nextX, nextW, intended, acked,
                    dPend, outcome, fault>>

C3s == bpc = "c3s" /\ DirSync /\ bpc' = "c4"
       /\ UNCHANGED <<ino, nIno, dirMem, wm, wmCo, recSt, wIno, nextX, nextW, intended, acked,
                      dPend, outcome>>

\* Create segment L+1 whole (TMP, fdatasync), then rename it to its name.
C4 == bpc = "c4" /\
  LET n == nIno + 1
      lrec == ino[dirMem[SegOf(dirMem, L)]].recs[L]
  IN /\ ino' = [i \in 1..n |-> IF i = n
                  THEN [first |-> L + 1, prev |-> Term(lrec), recs |-> EmptyRecs]
                  ELSE ino[i]]
     /\ nIno' = n
     /\ pend' = Append(pend, [name |-> L + 1, nonce |-> n])
     /\ dirMem' = [dirMem EXCEPT ![L + 1] = n]
     /\ wIno' = n /\ bpc' = "c4s"
     /\ UNCHANGED <<dirDur, wm, wmCo, recSt, nextX, nextW, intended, acked, dPend, outcome, fault>>

C4s == bpc = "c4s" /\ DirSync /\ bpc' = (IF MUT = "LowerAfterClear" THEN "c8" ELSE "c7")
       /\ UNCHANGED <<ino, nIno, dirMem, wm, wmCo, recSt, wIno, nextX, nextW, intended, acked,
                      dPend, outcome>>

\* Step 2.7: read record L from the media, lower the watermark.
C7 == bpc = "c7" /\
  LET lrec == ino[dirMem[SegOf(dirMem, L)]].recs[L]
      low  == [lsn |-> L, e |-> lrec, nonce |-> wIno]
  IN IF lrec = <<0, 0>>
     THEN /\ outcome' = [st |-> "refused", log |-> <<>>] /\ bpc' = "done"
          /\ UNCHANGED <<ino, nIno, dirDur, dirMem, pend, wm, wmCo, recSt, wIno, nextX, nextW,
                         intended, acked, dPend, fault>>
     ELSE /\ \/ wm' = low /\ fault' = fault
             \/ FaultOn /\ fault = "none" /\ wm' = wm /\ fault' = "lostWm"
          /\ bpc' = (IF MUT = "LowerAfterClear" THEN "app" ELSE "c8")
          /\ UNCHANGED <<ino, nIno, dirDur, dirMem, pend, wmCo, recSt, wIno, nextX, nextW,
                         intended, acked, dPend, outcome>>

C8 == bpc = "c8" /\ recSt' = "NONE"
      /\ bpc' = (IF MUT = "LowerAfterClear" THEN "c7" ELSE "app")
      /\ UNCHANGED <<ino, nIno, dirDur, dirMem, pend, wm, wmCo, wIno, nextX, nextW, intended,
                     acked, dPend, outcome, fault>>

\* Appends. Data(x) is issued, then completes (lands, or is lost by the fault); the
\* watermark for x follows its completion and that of every earlier write; then the
\* acknowledgement. The entry is the new primary's, or (V4) the identical old entry.
Done(x) == x < nextX /\ ~\E w \in dPend : w.x <= x

AppData == bpc = "app" /\ nextX <= MaxLsn /\ (IF Pipe THEN Cardinality(dPend) < 2 ELSE dPend = {}) /\
  \E e \in {New(nextX)} \cup (IF nextX <= 3 THEN {Old(nextX)} ELSE {}) :
    /\ (Len(intended) > L /\ Term(intended[Len(intended)]) = 2) => e = New(nextX)
    /\ dPend' = dPend \cup {[x |-> nextX, e |-> e]}
    /\ intended' = Append(intended, e) /\ nextX' = nextX + 1
    /\ UNCHANGED <<ino, nIno, dirDur, dirMem, pend, wm, wmCo, recSt, bpc, wIno, nextW,
                   acked, outcome, fault>>

DataLand == bpc = "app" /\ \E w \in dPend :
  /\ \/ ino' = [ino EXCEPT ![wIno].recs[w.x] = w.e] /\ fault' = fault
     \/ FaultOn /\ fault = "none" /\ ino' = ino /\ fault' = "lostData"
  /\ dPend' = dPend \ {w}
  /\ acked' = (IF MUT = "AckBeforeWm" THEN acked \cup {<<w.x, w.e>>} ELSE acked)
  /\ UNCHANGED <<nIno, dirDur, dirMem, pend, wm, wmCo, recSt, bpc, wIno, nextX, nextW,
                 intended, outcome>>

AppWm == bpc = "app" /\ Done(nextW) /\
  LET x == nextW
      e == intended[x]
  IN /\ \/ wm' = [lsn |-> x, e |-> e, nonce |-> (IF MUT = "BindOnce" THEN 0 ELSE wIno)] /\ fault' = fault
        \/ FaultOn /\ fault = "none" /\ wm' = wm /\ fault' = "lostWm"
     /\ acked' = acked \cup {<<x, e>>}
     /\ nextW' = x + 1
     /\ UNCHANGED <<ino, nIno, dirDur, dirMem, pend, wmCo, recSt, bpc, wIno, nextX, intended,
                    dPend, outcome>>

BCrash(crashOk) == crashOk /\ bpc \notin {"done", "stop"} /\
  \E k \in 0..Len(pend) : \E K \in SUBSET (1..Len(pend)) :
    /\ Cardinality(K) = k
    /\ LET sub == [i \in 1..k |-> pend[CHOOSE j \in K : Cardinality({m \in K : m < j}) = i - 1]]
       IN dirDur' = ApplySeq(dirDur, sub)
    /\ dirMem' = dirDur'
    /\ \E D \in SUBSET dPend :
         ino' = [i \in DOMAIN ino |-> IF i = wIno
                   THEN [ino[i] EXCEPT !.recs = [x \in 1..MaxLsn |->
                           IF \E w \in D : w.x = x THEN (CHOOSE w \in D : w.x = x).e
                           ELSE ino[i].recs[x]]]
                   ELSE ino[i]]
    /\ pend' = <<>> /\ wmCo' = NoWm /\ bpc' = "rec" /\ dPend' = {}
    /\ UNCHANGED <<nIno, wm, recSt, wIno, nextX, nextW, intended, acked, outcome, fault>>

\* Recovery: rerun completion while PENDING, else scan and check (terminal).
BRecover == bpc = "rec" /\
  IF recSt = "PENDING"
  THEN /\ bpc' = "c1"
       /\ UNCHANGED <<ino, nIno, dirDur, dirMem, pend, wm, wmCo, recSt, wIno, nextX, nextW,
                      intended, acked, dPend, outcome, fault>>
  ELSE /\ outcome' = Check(dirDur) /\ bpc' = "done"
       /\ UNCHANGED <<ino, nIno, dirDur, dirMem, pend, wm, wmCo, recSt, wIno, nextX, nextW,
                      intended, acked, dPend, fault>>

\* ============================== spec ======================================
Init == MutOk /\ RecInit /\ WmInit /\ fault = "none" /\ crashes = 0

CrashOk == crashes < MaxCrash

RecNext ==
  \/ Land /\ UNCHANGED <<wmVars, crashes>>
  \/ LandFault /\ UNCHANGED <<wmVars, crashes>>
  \/ Foreign /\ UNCHANGED <<wmVars, crashes>>
  \/ Synced /\ UNCHANGED <<wmVars, crashes>>
  \/ RunWrite /\ UNCHANGED <<wmVars, crashes>>
  \/ RCrash(CrashOk) /\ crashes' = crashes + 1 /\ UNCHANGED wmVars
  \/ Recover /\ UNCHANGED <<wmVars, crashes>>

WmNext ==
  \/ WmCoLand \/ Intent \/ C1 \/ C3 \/ C3s \/ C4 \/ C4s \/ C7 \/ C8 \/ AppData \/ DataLand \/ AppWm
  \/ BRecover

Next ==
  \/ PART = "rec" /\ RecNext
  \/ PART = "wm" /\ \/ WmNext /\ UNCHANGED <<recVars, crashes>>
                    \/ BCrash(CrashOk) /\ crashes' = crashes + 1 /\ UNCHANGED recVars

Spec == Init /\ [][Next]_vars

\* ============================== properties ================================
\* rec: recovery never adopts a value older than the newest acted-on one.
NoRollback == PART = "rec" /\ rpc = "write" /\ stage \in {"r4", "init1", "bootFirst"} =>
                \/ chosen[1] > acted[1]
                \/ chosen = acted
                \/ acted = Bad
\* rec: a refusal only after a media fault (covers ties).
RecNoCrashRefusal == refusedR => fault # "none"

\* wm: every acknowledged entry of the current log survives, or recovery refuses.
NoSilentLoss == outcome.st = "ok" =>
                  \A a \in Required : a[1] <= Len(outcome.log) /\ outcome.log[a[1]] = a[2]
\* wm: crashes alone never refuse.
WmNoCrashRefusal == outcome.st = "refused" => fault # "none"
\* wm: crashes alone give exactly a prefix of the current log.
CrashOnlyExact == outcome.st = "ok" /\ fault = "none" =>
                    /\ Len(outcome.log) <= Len(intended)
                    /\ \A x \in 1..Len(outcome.log) : outcome.log[x] = intended[x]
\* wm: anything recovered that differs from the current log lies above the watermark.
RevivalAboveW == outcome.st = "ok" =>
                   \A x \in 1..Len(outcome.log) :
                     (x > Len(intended) \/ outcome.log[x] # intended[x]) => x > wm.lsn

\* wm: a lost L + 1 binding never brings back a voided entry unrefused (design 3(e):
\* "That would be silent"; the nonce check closes it).
NoBindRevival == outcome.st = "ok" /\ fault = "lostBind" =>
                   \A x \in 1..Len(outcome.log) : x <= Len(intended) /\ outcome.log[x] = intended[x]

Safety == NoRollback /\ RecNoCrashRefusal /\ NoSilentLoss /\ WmNoCrashRefusal
          /\ CrashOnlyExact /\ RevivalAboveW /\ NoBindRevival

\* Reachability probes (each expected to be violated, i.e. reached).
PRecRule4     == ~(PART = "rec" /\ stage = "r4" /\ pw # {})
PRecTie       == ~(refusedR)
PRecReinit    == ~(PART = "rec" /\ crashes > 0 /\ stage = "init1" /\ rpc = "write")
PRecFault(k)  == ~(fault = k /\ crashes > 0 /\ rpc = "run")
PRecLost      == PRecFault("lost")
PRecTorn      == PRecFault("torn")
PRecMisdir    == PRecFault("misdirected")
PRecForeign   == PRecFault("foreign")
PRecTop       == ~(PART = "rec" /\ acted[1] = MaxGen)
PWmFault(k)   == ~(fault = k /\ outcome.st # "none")
PWmLostData   == PWmFault("lostData")
PWmLostWm     == PWmFault("lostWm")
PWmLostUnlink == PWmFault("lostUnlink")
PWmLostBind   == PWmFault("lostBind")
PWmStray      == ~(outcome.st = "refused" /\ fault = "none")  \* expected reached only if Pipe
PRecActed     == ~(PART = "rec" /\ acted[1] >= 3)              \* vacuity check per rec lane
PWmOkLostBind == ~(outcome.st = "ok" /\ fault = "lostBind")
PWmBindRefuse == ~(outcome.st = "refused" /\ fault = "lostBind" /\ nextW > L + 1)
PWmDone       == ~(outcome.st # "none")                         \* vacuity check per wm lane
PWmRefused    == ~(outcome.st = "refused")
PWmRerun      == ~(PART = "wm" /\ crashes > 0 /\ bpc = "c7")
PWmRevived    == ~(outcome.st = "ok" /\
                   \E x \in 1..Len(outcome.log) : x > Len(intended) \/ outcome.log[x] # intended[x])
PWmOldReturn  == ~(outcome.st = "ok" /\ Len(outcome.log) > L /\ Len(intended) > L
                   /\ outcome.log[L + 1] = Old(L + 1) /\ <<L + 1, Old(L + 1)>> \in acked)
PWmNewAcked   == ~(outcome.st = "ok" /\ Len(outcome.log) = MaxLsn
                   /\ \A x \in L + 1 .. MaxLsn : <<x, New(x)>> \in acked)
==============================================================================
