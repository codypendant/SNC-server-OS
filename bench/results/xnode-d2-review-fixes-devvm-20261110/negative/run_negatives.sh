#!/bin/bash
# D.2 negative mutations (docs/CROSS_NODE_EXCHANGE_DESIGN.md 8.2), after the PR #95
# review (copied from bench/results/xnode-d2-validation-devvm-20261110/negative/,
# with ND10 and ND24 redefined and ND39-ND41 added): each
# mutation of the cross-node accounting code must make the named test_xnode
# case(s) fail. As for D.1 (bench/results/intake-d1-validation-devvm-20261109/
# negative/run_negatives.sh): a patched copy of the repository in WORK
# (outside the repo), one Release build per mutation, then the named tests;
# nothing of the mutation is in the shipped source. CHECK_ONLY=1: only check
# that every pattern applies. FROM_WORKTREE=1: iterate from the working
# tree with kept builds (not for recorded runs).
# Usage: run_negatives.sh REPO WORK OUT_DIR [ND...]   (no ND: all)
set -u
REPO=${1:?repo}; WORK=${2:?work dir}; OUT=$(realpath -m "${3:?out dir}")
shift 3
ONLY=" $* "
mkdir -p "$OUT" "$WORK"
echo "base $(git -C "$REPO" rev-parse HEAD) $(date -u +%FT%TZ)" >> "$OUT/summary.txt"
NL=src/core/node_link.cpp
SH=src/core/shard.cpp
GEN=src/core/links_generation.cpp
clean() {  # the mutation's copy and build, by name
  rm -rf "${WORK:?}/src-${1:?}" "${WORK:?}/build-${1:?}"
}
mutate() {  # name "tests" file old new [old new | @file ...]
  local name=$1 tests=$2 file=$3
  shift 3
  if [ "$ONLY" != "  " ] && [[ "$ONLY" != *" $name "* ]]; then return; fi
  if [ -n "${FROM_WORKTREE:-}" ]; then
    # Iterating on tests: the working tree's tracked files, and the build
    # kept (incremental); the recorded runs use HEAD and clean builds.
    rm -rf "${WORK:?}/src-${name:?}"
    mkdir -p "$WORK/src-$name"
    (cd "$REPO" && git ls-files -z | tar --null -T - -cf - | tar -x -C "$WORK/src-$name")
  else
    clean "$name"
    mkdir -p "$WORK/src-$name" && git -C "$REPO" archive HEAD | tar -x -C "$WORK/src-$name"
  fi
  python3 - "$WORK/src-$name" "$file" "$@" <<'PY'
import sys
root, first, rest = sys.argv[1], sys.argv[2], sys.argv[3:]
f = first
i = 0
while i < len(rest):
    if rest[i].startswith('@'):
        f = rest[i][1:]; i += 1; continue
    old, new = rest[i], rest[i + 1]; i += 2
    p = root + '/' + f
    s = open(p).read()
    assert s.count(old) == 1, ("pattern not unique", f, old)
    open(p, 'w').write(s.replace(old, new))
PY
  if [ $? -ne 0 ]; then echo "$name MUTATION NOT APPLIED (pattern did not match)" >> "$OUT/summary.txt"; clean "$name"; return; fi
  if [ -n "${CHECK_ONLY:-}" ]; then echo "$name applies" >> "$OUT/summary.txt"; clean "$name"; return; fi
  [ -f "$WORK/build-$name/build.ninja" ] ||
    cmake -S "$WORK/src-$name" -B "$WORK/build-$name" -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
  if ! cmake --build "$WORK/build-$name" --target test_xnode -j8 > "$OUT/$name-build.log" 2>&1; then
    echo "$name BUILD FAILED" >> "$OUT/summary.txt"; clean "$name"; return
  fi
  rm -f -- "${OUT:?}/${name:?}-build.log"
  local caught="" t rc
  for t in $tests; do
    # A failing mutant leaves its data directories: under TMPDIR, inside
    # the work directory, removed with it.
    rm -rf "${WORK:?}/tmp-${name:?}" && mkdir -p "$WORK/tmp-$name"
    (cd "$WORK/build-$name" && ulimit -f 2097152 && TMPDIR="$WORK/tmp-$name" timeout 600 ./test_xnode "$t" > "$OUT/$name-$t.log" 2>&1)
    rc=$?
    rm -rf "${WORK:?}/tmp-${name:?}"
    grep -v "^snc: " "$OUT/$name-$t.log" | tail -4 > "$OUT/$name-$t.tail"
    echo "$name $t rc=$rc (expected non-zero): $(grep -m1 -E 'CHECK failed|timed out' "$OUT/$name-$t.log" | cut -c1-240)" >> "$OUT/summary.txt"
    rm -f -- "${OUT:?}/${name:?}-${t:?}.log"
    [ $rc -ne 0 ] && caught="$caught $t"
  done
  echo "$name => ${caught:-NOT CAUGHT}" >> "$OUT/summary.txt"
  if [ -n "${FROM_WORKTREE:-}" ]; then rm -rf "${WORK:?}/src-${name:?}"; else clean "$name"; fi
}

# ND1 end the exchange at push (D.1): the counted push does not count.
mutate ND1 "X1 X2" $NL \
  "  set_count(e, e.count + 1);
  ++o.pushed;" \
  "  ++o.pushed;"
# ND2 omit the END on one disposition: a stale drop (route_emit), the
# disposition X12 exercises; ND2b the misroute drop (on_link_frame).
mutate ND2 "X12 X8 X3" $SH \
  "      ++stats_.emits_stale;
      release_link(e);
      end_item(e);
      return;
    }
    if (group != group_ && e.remote) {" \
  "      ++stats_.emits_stale;
      release_link(e);
      if (!e.tagged) end_item(e);
      return;
    }
    if (group != group_ && e.remote) {"
mutate ND2b "X8 X3 X17" $SH \
  "    ++s->stats_.node_emits_misrouted;
    s->end_item(e);" \
  "    ++s->stats_.node_emits_misrouted;
    if (!e.tagged) s->end_item(e);"
# ND3 apply a SYNC from an instance other than the identified one.
mutate ND3 "X30 X13" $NL \
  "  if (!names_cur || !same) {" \
  "  if (!names_cur) {"
# ND4 no current SYNC on a new Hello.
mutate ND4 "X3" $NL \
  "  ent(l, h.member).sync_owed = true;  // a new Hello from it (2.5)" \
  ""
# ND5 a counted reply does not decrement.
mutate ND5 "X1" $NL \
  "  set_count(e, e.count - 1);
  ++ctr_.replies_returned;" \
  "  ++ctr_.replies_returned;"
# ND6 both an END and a counted reply for one item.
mutate ND6 "X3" $NL \
  "  f.pad[1] = COUNTED_BY_RECEIVER;
  ++ctr_.replies_pushed;" \
  "  f.pad[1] = COUNTED_BY_RECEIVER;
  ++e.end_owed;
  ++ctr_.replies_pushed;"
# ND7 keep end_owed when pushing a SYNC (the ENDs it subsumes go again).
mutate ND7 "X4" $NL \
  "    const u64 cleared = e.end_owed;
    e.end_owed = 0;
    e.sync_owed = false;" \
  "    const u64 cleared = 0;
    e.sync_owed = false;"
# ND8 no silence close.
mutate ND8 "X5" $NL \
  "    close_in(j, /*reset=*/true);
    ++silence_closes_;" \
  "    continue;"
# ND9 release on Out-side loss with the In side up (the count zeroed).
mutate ND9 "X5 X4" $NL \
  "  Out& o = *out_[i];
  o.at = (o.at + 1) % o.members.size();" \
  "  Out& o = *out_[i];
  if (o.identified) set_count(ent(i, o.ack_member), 0);
  o.at = (o.at + 1) % o.members.size();"
# ND10 (after the PR #95 review: the rev1 adoption branch is now the
# invariant I1 check, design 2.9.1, so the mutation breaks a premise of
# its proof instead): the source half of the staleness test removed. A
# follow-up whose source moved after its epoch is no longer stale.
mutate ND10 "X8 X12 X17" $SH \
  "  return !e.source.is_null() && e.epoch < moved_at_[slot_of(e.source)];" \
  "  return false;"
# ND11 no END for a purge of a link (tagged items queued on an unbound one).
mutate ND11 "X12" $SH \
  "        release_link(q.front());
        end_item(q.front());
        q.pop_front();
      }
      continue;" \
  "        release_link(q.front());
        if (!q.front().tagged) end_item(q.front());
        q.pop_front();
      }
      continue;"
# ND12 no incarnation check on END or SYNC.
mutate ND12 "X30 X13 X16 X25" $NL \
  "  if (end_target(fr) != opt_.incarnation || n == 0) {  // another incarnation's, or malformed (n >= 1, 2.2)
    ++ctr_.ignored_frames;
    return;
  }
  Entry& e = ent(f.link, f.member);
  if (e.serial != f.serial || e.inc != f.incarnation) {
    ++ctr_.stale_tags;
    return;
  }" \
  "  if (n == 0) {
    ++ctr_.ignored_frames;
    return;
  }
  Entry& e = ent(f.link, f.member);" \
  "  if (fr.target.lo != opt_.incarnation) {
    ++ctr_.ignored_frames;
    return;
  }
  Out& o = *out_[f.link];" \
  "  Out& o = *out_[f.link];"
# ND13 silence close (Out reset) while paused: the peer's paused bit ignored.
mutate ND13 "X11" $NL \
  "  p.paused = (f.op & 1u) != 0;" \
  "  p.paused = false;"
# ND14 carry pushed across a reconnect.
mutate ND14 "X3" $NL \
  "  o.mismatch_since_ns = o.r_stuck_since_ns = o.last_r = o.pushed = o.steer_since_ns = o.hb_ns = 0;" \
  "  o.mismatch_since_ns = o.r_stuck_since_ns = o.last_r = o.steer_since_ns = o.hb_ns = 0;"
# ND15 bind from the reverse SYNC alone (no HelloAck needed).
mutate ND15 "X13 X20 X27" $NL \
  "  if (!opt_.accounting || o.state != Out::State::UP || o.is_bound || !o.identified || !o.synced) return;" \
  "  if (!opt_.accounting || o.state != Out::State::UP || o.is_bound || !o.synced) return;
  if (!o.identified) {
    o.identified = true;
    o.ack_member = o.sync_member;
    o.ack_incarnation = o.sync_incarnation;
  }"
# ND16 release without quarantine: no Out reset, counted DATA continues.
mutate ND16 "X14" $NL \
  "  if (o.state != Out::State::DOWN && names) {" \
  "  if (false && names) {"
# ND17 one end_owed per link, not per entry: any binding takes all of it.
mutate ND17 "X16" $NL \
  "    Entry& e = ent(l, o.ack_member);
    if (e.end_owed == 0 || e.inc != o.ack_incarnation) continue;" \
  "    Entry& e = ent(l, o.ack_member);
    for (u16 mm = 0; mm < link_members_[l]; ++mm) {
      if (mm == o.ack_member) continue;
      e.end_owed += ent(l, mm).end_owed;
      ent(l, mm).end_owed = 0;
    }
    if (e.end_owed == 0 || e.inc != o.ack_incarnation) continue;"
# ND18 no eager retirement purge.
mutate ND18 "X31 X15" $SH \
  "  s->purge_tagged(static_cast<long>(link), member, s->links_->entry(link, member).serial);" \
  "  (void)s;
  (void)link;
  (void)member;"
# ND19 retire every other member's entry when a member appears (rev1).
mutate ND19 "X15 X16" $NL \
  "  hi = inc;
  Entry& e = ent(l, m);" \
  "  hi = inc;
  for (u16 mm = 0; mm < link_members_[l]; ++mm) {
    if (mm == m || ent(l, mm).inc == 0) continue;
    if (opt_.on_retire) opt_.on_retire(opt_.ctx, l, mm, ent(l, mm).inc);
    set_count(ent(l, mm), 0);
    ent(l, mm).serial++;
  }
  Entry& e = ent(l, m);" \
  "    const u64 old = hi;
    hi = inc;" \
  "    const u64 old = hi;
    hi = inc;
    for (u16 mm = 0; mm < link_members_[l]; ++mm) {
      if (mm == m || ent(l, mm).inc == 0) continue;
      if (opt_.on_retire) opt_.on_retire(opt_.ctx, l, mm, ent(l, mm).inc);
      set_count(ent(l, mm), 0);
      ent(l, mm).serial++;
    }"
# ND20 In replacement per (group, shard), not per member.
mutate ND20 "X15 X13 X20" $NL \
  "in_[j]->from.link == l && in_[j]->from.member == h.member) close_in(j);" \
  "in_[j]->from.link == l) close_in(j);"
# ND21 queue on an unbound link (no purge).
mutate ND21 "X22" $SH \
  "    if (!links_->ready(i)) {
      // Lost with the link" \
  "    if (!links_->ready(i) && links_->accounting()) {
      node_left = true;
      continue;
    }
    if (!links_->ready(i)) {
      // Lost with the link"
# ND22 no resume_all at binding.
mutate ND22 "X22" $NL \
  "  connected_ = true;  // resume_all, re-sending what was purged while unbound" \
  ""
# ND23 accept a new incarnation while the old one's In connection is live.
mutate ND23 "X21" $NL \
  "      if (now_ns - in_[j]->last_read_ns < opt_.dead_ns) {
        ++m_conflicts_;" \
  "      if (false) {
        ++m_conflicts_;"
# ND24 (after the PR #95 review: the in-node hand-off branch is now the
# invariant I2 check, 2.9.1; the mutation breaks a premise of its proof):
# no staleness test when a follow-up runs.
mutate ND24 "X8 X12 X17" $SH \
  "  if (stale(e)) {
    ++stats_.emits_stale;
    release_link(e);" \
  "  if (false) {
    ++stats_.emits_stale;
    release_link(e);"
# ND25 leave quarantine on a HelloAck alone.
mutate ND25 "X14" $NL \
  "  try_bind(l);
}

void NodeLinks::try_bind(u32 l) {" \
  "  {
    Entry& q = ent(l, o.ack_member);
    if (opt_.accounting && q.state == EntryState::QUARANTINED) {
      q.state = EntryState::ACTIVE;
      if (!q.gates) {
        q.gates = true;
        xnode_live_ += q.count;
      }
      o.is_bound = true;
    }
  }
  try_bind(l);
}

void NodeLinks::try_bind(u32 l) {"
# ND26 (stand-in) the rev2-draft split counters: a SYNC during quarantine
# does not touch the count (it stays stuck).
mutate ND26 "X5" $NL \
  "  Entry& e = ent(f.link, f.member);
  const u64 before = e.count;" \
  "  Entry& e = ent(f.link, f.member);
  if (e.state == EntryState::QUARANTINED) {
    ++ctr_.syncs_exact;
    return;
  }
  const u64 before = e.count;"
# ND27 the rev2 FINAL rule: a SYNC naming the connection before the current
# one applied as k + (pushed - R).
mutate ND27 "X24 X3" $NL \
  "fr.source.hi == o.conn && fr.op == o.conn_seq;" \
  "(fr.op == o.conn_seq || fr.op + 1 == o.conn_seq);"
# ND28 apply a SYNC naming any older connection (stale).
mutate ND28 "X24" $NL \
  "  const bool names_cur = o.state == Out::State::UP && o.conn != 0 && fr.source.hi == o.conn && fr.op == o.conn_seq;" \
  "  const bool names_cur = o.state == Out::State::UP && o.conn != 0;"
# ND29 counted DATA after the HelloAck, before the binding SYNC.
mutate ND29 "X3 X24" $NL \
  "bool NodeLinks::ready(u32 l) const { return opt_.accounting ? bound(l) : up(l); }" \
  "bool NodeLinks::ready(u32 l) const { return opt_.accounting ? (bound(l) || (up(l) && out_[l]->identified)) : up(l); }"
# ND30 accept a new incarnation after silence without fencing the old socket.
mutate ND30 "X25" $NL \
  "        close_in(j, /*reset=*/true);
        ++fenced_;
      }
    }
    Out& o = *out_[l];" \
  "        ++fenced_;
      }
    }
    Out& o = *out_[l];"
# ND31 reset blocked_since when the old instance's links close.
mutate ND31 "X19" $NL \
  "void NodeLinks::close_in(u32 i, bool reset) {
  if (!in_[i]->muted)" \
  "void NodeLinks::close_in(u32 i, bool reset) {
  if (in_[i]->hello) peer_[mbase_[in_[i]->from.link] + in_[i]->from.member].blocked_since_ns = 0;
  if (!in_[i]->muted)"
# ND32 no not-bound quarantine trigger.
mutate ND32 "X26" $NL \
  "      } else if (e.count != 0 && now_ns - e.bound_ns >= rel) {" \
  "      } else if (false) {"
# ND33 ignore a binding SYNC read before its HelloAck.
mutate ND33 "X27" $NL \
  "                                  : true;
  if (!names_cur || !same) {" \
  "                                  : false;
  if (!names_cur || !same) {"
# ND34 send the blocked bit only in idle heartbeats.
mutate ND34 "X19" $NL \
  "  for (u32 l = 0; l < out_.size(); ++l) push_heartbeat(l, now_ns_);" \
  "" \
  "        (now_ns - o.last_push_ns >= opt_.hb_ns || o.blocked_sent != (blocked_ ? 1 : 0))) {" \
  "        (now_ns - o.last_push_ns >= opt_.hb_ns)) {"
# ND35 allow BOUND to an entry with blocked_since set.
mutate ND35 "X19" $NL \
  "  if (peer_blocked_since(l, o.ack_member) != 0) return;  // never bound to a blocked instance (2.12 A)" \
  ""
# ND36 insert the SYNC ahead of frames already encoded in Out::buf (at the
# next frame boundary, so the stream stays well-formed).
mutate ND36 "X28" $NL \
  "    e.sync_owed = false;
    push(l, f);" \
  "    e.sync_owed = false;
    {
      const std::size_t at = o.off + (o.buf.size() - o.off) % sizeof(Frame);
      const u8* p = reinterpret_cast<const u8*>(&f);
      o.buf.insert(o.buf.begin() + static_cast<std::ptrdiff_t>(at), p, p + sizeof(f));
    }"
# ND37 the rev3 incarnation: CLOCK_REALTIME milliseconds plus random low bits.
mutate ND37 "X29" $GEN \
  "#include <unistd.h>" \
  "#include <time.h>
#include <unistd.h>

#include <cstdlib>" \
  "  std::lock_guard<std::mutex> g(mu_);
  if (source_ == Source::TEST) {" \
  "  std::lock_guard<std::mutex> g(mu_);
  {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    const u64 ms = debug_clock_ms ? debug_clock_ms() : u64(ts.tv_sec) * 1000 + u64(ts.tv_nsec) / 1000000;
    out = (ms << 16) | (u64(::random()) & 0xffff);
    value_ = out;
    return true;
  }
  if (source_ == Source::TEST) {"
# ND38 (implementation finding, X15) Out-side evidence from one stale
# heartbeat: the T_dead clock runs without heartbeats that keep showing it.
mutate ND38 "X32 X15" $NL \
  "  if (o.hb_ns != 0 && now_ns - o.hb_ns > opt_.dead_ns) o.mismatch_since_ns = o.r_stuck_since_ns = 0;" \
  "" \
  "    if ((o.mismatch_since_ns != 0 && o.hb_ns - o.mismatch_since_ns >= opt_.dead_ns) ||
        (o.r_stuck_since_ns != 0 && o.hb_ns - o.r_stuck_since_ns >= opt_.dead_ns)) {" \
  "    if ((o.mismatch_since_ns != 0 && now_ns - o.mismatch_since_ns >= opt_.dead_ns) ||
        (o.r_stuck_since_ns != 0 && now_ns - o.r_stuck_since_ns >= opt_.dead_ns)) {"
# ND39 (PR #95 review item 2) the one-sided retry: a blocked bit not yet
# carried is retried only while blocked; a clear waits for idleness.
mutate ND39 "X34" $NL \
  "        (now_ns - o.last_push_ns >= opt_.hb_ns || o.blocked_sent != (blocked_ ? 1 : 0))) {" \
  "        (now_ns - o.last_push_ns >= opt_.hb_ns || (blocked_ && o.blocked_sent != 1))) {"
# ND40 (review item 1) no lock on an explicit generation file.
mutate ND40 "X33" $GEN \
  "  if (own_lock) {" \
  "  if (false) {"
# ND41 (review item 3) the rev4 table's END mapping (n in op, the target
# incarnation in source.hi), in the one encoder and decoder: sender,
# receiver and proxy agree with each other, not with the documented layout.
mutate ND41 "X35" src/core/node_link.h \
  "    f.source.hi = n;
    f.target.lo = target_incarnation;" \
  "    f.op = static_cast<u32>(n);
    f.source.hi = target_incarnation;" \
  "  static u64 end_n(const Frame& f) { return f.source.hi; }
  static u64 end_target(const Frame& f) { return f.target.lo; }" \
  "  static u64 end_n(const Frame& f) { return f.op; }
  static u64 end_target(const Frame& f) { return f.source.hi; }"
echo "done $(date -u +%FT%TZ)" >> "$OUT/summary.txt"
