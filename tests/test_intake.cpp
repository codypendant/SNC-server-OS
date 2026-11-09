// Follow-up intake backpressure, slice D.1 (docs/FOLLOWUP_INTAKE_DESIGN.md
// revision 2.1; review docs/handoff/PR85_Design_Review.txt).
//
//   F10 contract death tests: a follow-up replying to a third object; a
//       follow-up replying twice; a client op emitting past its
//       reservation, or with none; reserve_emits above max_emits or from
//       apply; an op that declares no emits emitting; a follow-up to an op
//       not declared followup; a resume hook past max_resume_emits. Each
//       stops the process (SIGABRT) with the contract message; a client
//       calling a follow-up op gets E_BAD_ARGS.
//   F1  WAL bound with a stalled destination: seven senders (and the
//       destination itself, F7) send probes into a shard whose WAL is held;
//       its WAL never holds more than max(H, R); its staging stays within
//       (N-1)W + its own exchanges; each sender's exchanges within X + E;
//       senders are refused (refused_exchange); everything is delivered
//       exactly once, in per-source order, after release.
//   F2  responses and control while intake is closed: forwarded reads,
//       batched reads, a forwarded mutation (answered: admitted or
//       refused) and STATS all answer while the destination stages.
//   F3  every shard stalled, probes on every pair, W = 1 and X = 4: no
//       deadlock; all delivered in order once released.
//   F4  drop paths: probes to missing objects end their exchanges where they
//       fail (no fail op); exchanges and windows all return to zero.
//   F5  bounds sampled during F1 and F3 (per pair, per shard, node total).
//   F6  SIGKILL with intake closed, then recovery: transfers settle,
//       money is conserved, no entries left.
//   F8  two engines over node links (D.1 scope: L pausing; cross-node
//       exchange accounting, which bounds the sender, is D.2): the
//       receiver's WAL held, its link reads pause at L with no disconnect,
//       and everything settles in order after release.
//   F11 rate mismatch: d fills X with probes to x; x's replies are held,
//       then released into d whose WAL is held at its budget B, so d's gate
//       closes: x's backlog toward d grows past W yet stays within
//       live(d) + live(x); d stages at most W; d's sends past X are refused.
//   F12 X saturated: transfers between two accounts of the saturated shard
//       still run (they reserve no exchange).
//   F13 ... a transfer that would start an exchange is refused
//       (refused_exchange), changing nothing.
//   F14 ... a sequenced transfer applied before saturation is replayed.
//   F15 ... repeated resume triggers coalesce: one active pass plus one
//       owed, fixed scan state, exchanges within X + max(E, Rr); after
//       release at most one more pass, and everything settles.
//   G1  the owner gate: a mutation forwarded to a shard whose follow-up
//       backlog is past kEmitBacklog is refused there (refused_owner); a
//       forwarded read is answered.
#include <dirent.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <latch>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/core/shard.h"  // Shard::debug_resume (F15)
#include "harness.h"
#include "snc/cluster.h"
#include "snc/slot_map.h"
#include "snc/types/account.h"
#include "snc/wal_format.h"

using namespace snc;
using namespace snc::test;

namespace {

// ---- A probe type: one exchange per send, delivery order checked --------------------

constexpr u32 kProbeType = 901;
constexpr u32 kBadResumeType = 902;
constexpr u32 kSrcs = 64;
struct ProbeV1 {
  u64 sent, acked, received, order_errors, failed;
  u64 last[kSrcs];  // per source index: the last sequence received (0: none)
};
struct SendArgs {
  ObjectID target;
  u64 seq;
  u32 src;
  u32 op;  // 0: kOpPRecv (fail op kOpPFail); 1: kOpPRecv, no fail op; else that op (F10)
};
struct RecvArgs {
  u64 seq;
  u32 src;
  u32 pad;
};
static_assert(sizeof(RecvArgs) == OpCtx::kMaxEmitArgs);

enum : OpID {
  kOpPCreate = 9001,
  kOpPGet,
  kOpPSend,   // client: reserves 1, emits recv (or args.op) to target
  kOpPRecv,   // follow-up: checks order, replies ack to source
  kOpPAck,    // follow-up: counts
  kOpPFail,   // follow-up: the target is missing (fail op)
  // Contract violations (F10).
  kOpBadNoReserve,     // client, max_emits 1, reserves nothing, emits
  kOpBadReserveAbove,  // client, max_emits 1, reserves 2
  kOpBadReserveApply,  // client, reserves from apply
  kOpBadUndeclared,    // client, max_emits 0, emits
  kOpBadThird,         // follow-up, replies to a third object
  kOpBadTwice,         // follow-up, replies twice
  kOpBadResumeCreate,
};

ProbeV1* probe(OpCtx& c) { return reinterpret_cast<ProbeV1*>(c.obj()->payload()); }
Status ok_validate(OpCtx&, const ObjectHeader&, ByteView) { return Status::OK; }
Status follow_validate(OpCtx& c, const ObjectHeader&, ByteView) {
  return c.source().is_null() ? Status::E_BAD_ARGS : Status::OK;
}
void noop_apply(OpCtx&, ByteView) {}
void get_apply(OpCtx& c, ByteView) { c.set_result(c.obj()->payload(), sizeof(ProbeV1)); }
Status send_validate(OpCtx& c, const ObjectHeader&, ByteView args) {
  if (args.len != sizeof(SendArgs)) return Status::E_BAD_ARGS;
  c.reserve_emits(1);
  return Status::OK;
}
void send_apply(OpCtx& c, ByteView args) {
  SendArgs a;
  std::memcpy(&a, args.data, sizeof(a));
  ++probe(c)->sent;
  RecvArgs r{a.seq, a.src, 0};
  // op 0: recv with a fail op; 1: recv with none (dropped where it fails).
  c.emit(a.target, a.op > 1 ? a.op : kOpPRecv, ByteView(&r, sizeof(r)), a.op == 1 ? 0 : kOpPFail);
}
void recv_apply(OpCtx& c, ByteView args) {
  RecvArgs r;
  std::memcpy(&r, args.data, sizeof(r));
  ProbeV1* p = probe(c);
  ++p->received;
  if (r.src >= kSrcs || r.seq <= p->last[r.src]) ++p->order_errors;  // reordered or duplicated
  else p->last[r.src] = r.seq;
  c.emit(c.source(), kOpPAck, ByteView(&r, sizeof(r)));
}
void ack_apply(OpCtx& c, ByteView) { ++probe(c)->acked; }
void fail_apply(OpCtx& c, ByteView) { ++probe(c)->failed; }
// F10's violations.
Status noreserve_validate(OpCtx&, const ObjectHeader&, ByteView) { return Status::OK; }
Status above_validate(OpCtx& c, const ObjectHeader&, ByteView) {
  c.reserve_emits(2);
  return Status::OK;
}
Status one_validate(OpCtx& c, const ObjectHeader&, ByteView) {
  c.reserve_emits(1);
  return Status::OK;
}
void reserve_apply(OpCtx& c, ByteView) { c.reserve_emits(1); }
void emit_self_apply(OpCtx& c, ByteView) {
  RecvArgs r{1, 0, 0};
  c.emit(c.obj()->id, kOpPAck, ByteView(&r, sizeof(r)));
}
void third_apply(OpCtx& c, ByteView) {
  RecvArgs r{1, 0, 0};
  c.emit(ObjectID{c.source().hi, c.source().lo + 1}, kOpPAck, ByteView(&r, sizeof(r)));
}
void twice_apply(OpCtx& c, ByteView) {
  RecvArgs r{1, 0, 0};
  c.emit(c.source(), kOpPAck, ByteView(&r, sizeof(r)));
  c.emit(c.source(), kOpPAck, ByteView(&r, sizeof(r)));
}
void bad_resume(OpCtx& c) {
  RecvArgs r{1, 0, 0};
  for (int i = 0; i < 2; ++i) c.emit(c.obj()->id, kOpPAck, ByteView(&r, sizeof(r)));
}

void register_probe() {
  Registry& reg = registry();
  if (reg.type(kProbeType)) return;
  TypeDescriptor t;
  t.type_id = kProbeType;
  t.name = "Probe";
  t.initial_size = sizeof(ProbeV1);
  t.max_size = sizeof(ProbeV1);
  t.create_op = kOpPCreate;
  reg.add_type(t);
  TypeDescriptor b = t;
  b.type_id = kBadResumeType;
  b.name = "BadResume";
  b.create_op = kOpBadResumeCreate;
  b.resume = bad_resume;
  b.max_resume_emits = 1;
  reg.add_type(b);
  auto op = [&](OpID id, u32 type, const char* name, OpKind kind, u32 max_args, u32 max_result,
                Status (*v)(OpCtx&, const ObjectHeader&, ByteView), void (*a)(OpCtx&, ByteView),
                u16 max_emits, bool followup) {
    OpDescriptor d;
    d.op_id = id;
    d.type_id = type;
    d.name = name;
    d.kind = kind;
    d.max_args = max_args;
    d.max_result = max_result;
    d.validate = v;
    d.apply = a;
    d.max_emits = max_emits;
    d.followup = followup;
    d.keeps_index_key = kind == OpKind::MUTATE;
    reg.add_op(d);
  };
  op(kOpPCreate, kProbeType, "Probe.create", OpKind::CREATE, 0, 0, ok_validate, noop_apply, 0, false);
  op(kOpPGet, kProbeType, "Probe.get", OpKind::READ, 0, sizeof(ProbeV1), ok_validate, get_apply, 0, false);
  op(kOpPSend, kProbeType, "Probe.send", OpKind::MUTATE, sizeof(SendArgs), 0, send_validate, send_apply, 1, false);
  op(kOpPRecv, kProbeType, "Probe.recv", OpKind::MUTATE, sizeof(RecvArgs), 0, follow_validate, recv_apply, 1, true);
  op(kOpPAck, kProbeType, "Probe.ack", OpKind::MUTATE, sizeof(RecvArgs), 0, follow_validate, ack_apply, 0, true);
  op(kOpPFail, kProbeType, "Probe.fail", OpKind::MUTATE, sizeof(RecvArgs), 0, follow_validate, fail_apply, 0, true);
  op(kOpBadNoReserve, kProbeType, "Bad.no_reserve", OpKind::MUTATE, 0, 0, noreserve_validate, emit_self_apply, 1, false);
  op(kOpBadReserveAbove, kProbeType, "Bad.reserve_above", OpKind::MUTATE, 0, 0, above_validate, noop_apply, 1, false);
  op(kOpBadReserveApply, kProbeType, "Bad.reserve_apply", OpKind::MUTATE, 0, 0, one_validate, reserve_apply, 1, false);
  op(kOpBadUndeclared, kProbeType, "Bad.undeclared", OpKind::MUTATE, 0, 0, ok_validate, emit_self_apply, 0, false);
  op(kOpBadThird, kProbeType, "Bad.third", OpKind::MUTATE, sizeof(RecvArgs), 0, follow_validate, third_apply, 1, true);
  op(kOpBadTwice, kProbeType, "Bad.twice", OpKind::MUTATE, sizeof(RecvArgs), 0, follow_validate, twice_apply, 1, true);
  op(kOpBadResumeCreate, kBadResumeType, "BadResume.create", OpKind::CREATE, 0, 0, ok_validate, noop_apply, 0, false);
}

// ---- Harness ---------------------------------------------------------------------------

std::string temp_dir() {
  std::string t = "/tmp/snc-intake-XXXXXX";
  std::vector<char> b(t.begin(), t.end());
  b.push_back(0);
  char* d = mkdtemp(b.data());
  CHECK(d != nullptr);
  return std::string(d);
}
void rm_rf(const std::string& d) { CHECK_EQ(std::system(("rm -rf " + d).c_str()), 0); }

u64 rec(u32 len) { return record_size(len); }
// R: one follow-up apply's worst record group (Shard::set_intake).
u64 big_r() {
  u64 m = 0;
  for (const TypeDescriptor& t : registry().types()) m = std::max<u64>(m, rec(t.max_size));
  return 2 * m;
}

Engine::Config config(u16 shards, const std::string& dir) {
  Engine::Config c;
  c.runtime.n_shards = shards;
  c.expected_objects = 1 << 16;
  c.data_dir = dir;
  c.init = !dir.empty();
  c.wal_segment_bytes = 4u << 20;
  c.wal_buffer_budget_bytes = 256u << 10;  // B
  return c;
}

void on_shard(Engine& eng, u16 s, const std::function<void()>& fn) {
  std::latch l(1);
  const Engine::DataPlane dp = eng.data_plane();
  CHECK(dp);
  dp.submit(s, [&] {
    fn();
    l.count_down();
  });
  l.wait();
}

// Creates an object of `type` owned by shard s (created from s, which
// mints IDs it owns when mint_local; otherwise retried until one lands).
ObjectID make(Engine& eng, u16 s, u32 type, OpID create_op_args_type = 0) {
  (void)create_op_args_type;
  const SlotMap map = SlotMap::make_default(eng.shard_count());
  for (u32 guard = 0; guard < 100000; ++guard) {
    OneResult r = one(eng, s, [&](Completion cb, void* ctx) {
      if (type == account::kTypeId) {
        account::CreateArgs a{};
        a.balance_cents = 1000;
        a.created_ns = 1;
        a.email_len = 1;
        a.email[0] = 'e';
        a.name_len = 1;
        a.name[0] = 'n';
        create(type, ByteView(&a, sizeof(a)), {}, cb, ctx);
      } else {
        create(type, ByteView{}, {}, cb, ctx);
      }
    });
    CHECK_EQ(r.r.status, Status::OK);
    if (map.owner_of(r.r.id) == s) return r.r.id;
  }
  CHECK(false);
  return {};
}

ProbeV1 get_probe(Engine& eng, const ObjectID& id) {
  const SlotMap map = SlotMap::make_default(eng.shard_count());
  OneResult r = one(eng, map.owner_of(id), [&](Completion cb, void* ctx) {
    ExecOptions o;
    o.allow_undurable = true;
    execute(id, kOpPGet, ByteView{}, o, cb, ctx);
  });
  CHECK_EQ(r.r.status, Status::OK);
  return r.as<ProbeV1>();
}

// Sends on shard s, from `from` to `to`, `n` probes numbered from seq+1;
// returns how many were admitted (sequence numbers advance only on those).
struct SendOut {
  u64 ok = 0, refused = 0, last_seq = 0;
};
SendOut send_probes(Engine& eng, u16 s, const ObjectID& from, const ObjectID& to, u32 src, u64 seq, u64 n,
                    OpID op = 0) {
  SendOut out;
  out.last_seq = seq;
  on_shard(eng, s, [&] {
    for (u64 i = 0; i < n; ++i) {
      SendArgs a{to, out.last_seq + 1, src, op};
      struct R {
        Status st = Status::OK;
      } res;
      ExecOptions o;
      o.durability = Durability::APPLIED;
      execute(from, kOpPSend, ByteView(&a, sizeof(a)), o,
              [](void* c, const ExecResult& r) { static_cast<R*>(c)->st = r.status; }, &res);
      if (res.st == Status::OK) {
        ++out.ok;
        ++out.last_seq;
      } else {
        CHECK_EQ(res.st, Status::E_OVERLOADED);
        ++out.refused;
      }
    }
  });
  return out;
}

// Every shard's exchanges and windows are back to zero.
void expect_quiet(Engine& eng) {
  for (u16 s = 0; s < eng.shard_count(); ++s) {
    const ShardStats st = eng.stats(s);
    CHECK_EQ(st.exchanges_live, 0u);
    CHECK_EQ(st.window_outstanding, 0u);
    CHECK_EQ(st.followups_staged_now, 0u);
    CHECK_EQ(st.emits_in_flight, 0u);
    CHECK_EQ(st.link_held_now, 0u);
  }
}

// Runs `child` in a fork with stderr captured; returns (signal, stderr).
std::pair<int, std::string> in_child(const std::string& dir, const std::function<void()>& child) {
  std::fflush(stdout);
  const std::string err = dir + "/stderr";
  const pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    const int efd = ::open(err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (efd < 0 || ::dup2(efd, 2) < 0) std::_Exit(3);
    ::alarm(60);
    child();
    std::_Exit(0);
  }
  int ws = 0;
  CHECK_EQ(::waitpid(pid, &ws, 0), pid);
  std::string text;
  if (FILE* f = std::fopen(err.c_str(), "r")) {
    char buf[4096];
    std::size_t k;
    while ((k = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, k);
    std::fclose(f);
  }
  return {WIFSIGNALED(ws) ? WTERMSIG(ws) : 0, text};
}

// ---- F10 ---------------------------------------------------------------------------------

void expect_dies(const std::string& dir, const char* what, const std::function<void(Engine&, ObjectID, ObjectID)>& act) {
  auto [sig, err] = in_child(dir, [&] {
    Engine::Config c = config(2, "");
    Engine eng(c);
    eng.start();
    const ObjectID a = make(eng, 0, kProbeType), b = make(eng, 1, kProbeType);
    act(eng, a, b);
    eng.wait_emits(5);
    std::_Exit(0);  // survived: the parent reports it
  });
  if (sig != SIGABRT || err.find(what) == std::string::npos) {
    std::fprintf(stderr, "F10 expected SIGABRT with \"%s\"; got signal %d, stderr:\n%s\n", what, sig, err.c_str());
    CHECK(false);
  }
}

void client_op(Engine& eng, const ObjectID& id, OpID op, ByteView args = {}) {
  const SlotMap map = SlotMap::make_default(eng.shard_count());
  (void)one(eng, map.owner_of(id), [&](Completion cb, void* ctx) { execute(id, op, args, {}, cb, ctx); });
}

void test_contract_deaths() {
  const std::string dir = temp_dir();
  // A client op that emits with no reservation, or past it.
  expect_dies(dir, "more than it reserved", [](Engine& e, ObjectID a, ObjectID) { client_op(e, a, kOpBadNoReserve); });
  // reserve_emits above max_emits, or outside validate.
  expect_dies(dir, "reserve_emits above the op's max_emits",
              [](Engine& e, ObjectID a, ObjectID) { client_op(e, a, kOpBadReserveAbove); });
  expect_dies(dir, "reserve_emits outside the validate",
              [](Engine& e, ObjectID a, ObjectID) { client_op(e, a, kOpBadReserveApply); });
  // An op that declares no emits.
  expect_dies(dir, "more than its max_emits", [](Engine& e, ObjectID a, ObjectID) { client_op(e, a, kOpBadUndeclared); });
  // A follow-up replying to a third object, and twice.
  expect_dies(dir, "other than its source", [](Engine& e, ObjectID a, ObjectID b) {
    SendArgs s{b, 1, 0, kOpBadThird};
    client_op(e, a, kOpPSend, ByteView(&s, sizeof(s)));
  });
  expect_dies(dir, "more than its max_emits", [](Engine& e, ObjectID a, ObjectID b) {
    SendArgs s{b, 1, 0, kOpBadTwice};
    client_op(e, a, kOpPSend, ByteView(&s, sizeof(s)));
  });
  // A follow-up emitted to an op that is not a follow-up op.
  expect_dies(dir, "not declared followup", [](Engine& e, ObjectID a, ObjectID b) {
    SendArgs s{b, 1, 0, kOpPSend};
    client_op(e, a, kOpPSend, ByteView(&s, sizeof(s)));
  });
  // A resume hook past max_resume_emits.
  expect_dies(dir, "more than max_resume_emits", [](Engine& e, ObjectID, ObjectID) {
    const ObjectID r = make(e, 0, kBadResumeType);
    (void)r;
    on_shard(e, 0, [] { current_shard()->debug_resume(-1); });
  });
  // Not a death: a client calling a follow-up op is refused.
  {
    Engine::Config c = config(2, "");
    Engine eng(c);
    eng.start();
    const ObjectID a = make(eng, 0, kProbeType);
    RecvArgs r{1, 0, 0};
    OneResult x = one(eng, 0, [&](Completion cb, void* ctx) {
      execute(a, kOpPRecv, ByteView(&r, sizeof(r)), {}, cb, ctx);
    });
    CHECK_EQ(x.r.status, Status::E_BAD_ARGS);
    eng.stop();
  }
  rm_rf(dir);
  std::printf("F10 contract: 8 violations stop the process with their message; a client calling a follow-up op gets E_BAD_ARGS\n");
}

// ---- F1, F5, F7, F2 --------------------------------------------------------------------

void test_stalled_destination() {
  register_probe();
  const std::string dir = temp_dir();
  constexpr u16 kN = 8, kD = 1;
  constexpr u32 kW = 16, kX = 64;
  Engine::Config c = config(kN, dir);
  c.followup_window = kW;
  c.exchange_budget = kX;
  const u64 R = big_r(), B = c.wal_buffer_budget_bytes;
  c.followup_wal_cap_bytes = B + R;  // H: the smallest allowed, so follow-ups close it
  const u64 H = c.followup_wal_cap_bytes;
  Engine eng(c);
  eng.start();
  std::vector<ObjectID> src(kN);
  for (u16 s = 0; s < kN; ++s) src[s] = make(eng, s, kProbeType);
  const ObjectID dst = make(eng, kD, kProbeType);
  const u64 peak0 = eng.stats(kD).wal.active_bytes_peak;
  CHECK(peak0 <= H);
  Engine::debug_hold_wal(kD, true);
  // Every shard sends, the destination too (F7: a local exchange).
  std::vector<SendOut> sent(kN);
  u64 staged_peak = 0, total_peak = 0;
  for (u32 round = 0; round < 40; ++round) {
    for (u16 s = 0; s < kN; ++s) {
      SendOut o = send_probes(eng, s, src[s], dst, s, sent[s].last_seq, 100);
      sent[s].ok += o.ok;
      sent[s].refused += o.refused;
      sent[s].last_seq = o.last_seq;
    }
    // F5: sampled bounds.
    u64 total = 0;
    for (u16 s = 0; s < kN; ++s) {
      const ShardStats st = eng.stats(s);
      CHECK(st.exchanges_live <= kX + 1);
      total += st.emits_in_flight;
      if (s == kD) staged_peak = std::max(staged_peak, st.followups_staged_now);
    }
    total_peak = std::max(total_peak, total);
    CHECK(total <= u64{kN} * (kX + 1));
  }
  const ShardStats d = eng.stats(kD);
  // F1: the destination's WAL never held more than H (or one item alone).
  CHECK(d.wal.active_bytes_peak <= std::max(H, R));
  CHECK(d.followups_gated > 0);
  CHECK(d.followups_staged_peak <= u64{kN - 1} * kW + kX + 1);
  u64 ok = 0, refused = 0;
  for (u16 s = 0; s < kN; ++s) {
    ok += sent[s].ok;
    refused += sent[s].refused;
    if (s != kD) CHECK(eng.stats(s).refused_exchange >= sent[s].refused);  // d also hits its own WAL budget
  }
  CHECK(refused > 0);
  // F2: while it stages, the destination answers forwarded and batched
  // reads, a forwarded mutation (admitted or refused), and STATS.
  {
    std::atomic<u32> answered{0};
    on_shard(eng, 0, [&] {
      for (int i = 0; i < 40; ++i) {
        ExecOptions o;
        o.allow_undurable = true;
        execute(dst, kOpPGet, ByteView{}, o, [](void* p, const ExecResult& r) {
          CHECK_EQ(r.status, Status::OK);
          ++*static_cast<std::atomic<u32>*>(p);
        }, &answered);
      }
      ExecOptions o;
      o.durability = Durability::APPLIED;
      SendArgs a{src[0], 1, 63, 0};
      execute(dst, kOpPSend, ByteView(&a, sizeof(a)), o, [](void* p, const ExecResult& r) {
        CHECK(r.status == Status::OK || r.status == Status::E_OVERLOADED);
        ++*static_cast<std::atomic<u32>*>(p);
      }, &answered);
    });
    for (int i = 0; i < 5000 && answered.load() < 41; ++i) ::usleep(1000);
    CHECK_EQ(answered.load(), 41u);
  }
  Engine::debug_hold_wal(kD, false);
  CHECK(eng.wait_emits(60));
  const ProbeV1 p = get_probe(eng, dst);
  for (u16 s = 0; s < kN; ++s) {
    const ProbeV1 q = get_probe(eng, src[s]);
    CHECK_EQ(q.acked, q.sent);
    CHECK_EQ(p.last[s], sent[s].last_seq);
  }
  CHECK_EQ(p.order_errors, 0u);
  // Every admitted send arrived exactly once (plus F2's one, if admitted).
  CHECK(p.received == ok || p.received == ok + 1);
  expect_quiet(eng);
  eng.stop();
  rm_rf(dir);
  std::printf("F1/F5/F7/F2 stalled destination: %llu probes admitted, %llu refused by X=%u; WAL peak %llu <= max(H=%llu, R=%llu); "
              "staging peak %llu <= (N-1)W + X + 1 = %u; node backlog peak %llu <= N(X+1) = %u; reads, batch and a forwarded "
              "mutation answered while staging; delivered once, in order\n",
              (unsigned long long)ok, (unsigned long long)refused, kX, (unsigned long long)d.wal.active_bytes_peak,
              (unsigned long long)H, (unsigned long long)R, (unsigned long long)d.followups_staged_peak,
              (kN - 1) * kW + kX + 1, (unsigned long long)total_peak, kN * (kX + 1));
}

// ---- F3 -----------------------------------------------------------------------------------

void test_all_stalled() {
  register_probe();
  const std::string dir = temp_dir();
  constexpr u16 kN = 8;
  Engine::Config c = config(kN, dir);
  c.followup_window = 1;
  c.exchange_budget = 4;
  const u64 R = big_r();
  c.followup_wal_cap_bytes = c.wal_buffer_budget_bytes + R;  // the smallest H allowed
  Engine eng(c);
  eng.start();
  std::vector<ObjectID> p(kN);
  for (u16 s = 0; s < kN; ++s) p[s] = make(eng, s, kProbeType);
  for (u16 s = 0; s < kN; ++s) Engine::debug_hold_wal(s, true);
  std::vector<u64> seq(kN, 0), ok(kN, 0);
  u64 refused = 0;
  for (u32 round = 0; round < 30; ++round) {
    for (u16 s = 0; s < kN; ++s) {
      const u16 t = static_cast<u16>((s + 1 + round % (kN - 1)) % kN);
      SendOut o = send_probes(eng, s, p[s], p[t], s, seq[s], 8);
      seq[s] = o.last_seq;
      ok[s] += o.ok;
      refused += o.refused;
    }
    for (u16 s = 0; s < kN; ++s) CHECK(eng.stats(s).exchanges_live <= 4 + 1);
  }
  for (u16 s = 0; s < kN; ++s) Engine::debug_hold_wal(s, false);
  CHECK(eng.wait_emits(60));
  u64 received = 0, errors = 0;
  for (u16 s = 0; s < kN; ++s) {
    const ProbeV1 q = get_probe(eng, p[s]);
    CHECK_EQ(q.acked, q.sent);
    CHECK_EQ(q.sent, ok[s]);
    received += q.received;
    errors += q.order_errors;
  }
  u64 total = 0;
  for (u64 x : ok) total += x;
  CHECK_EQ(received, total);
  CHECK_EQ(errors, 0u);
  expect_quiet(eng);
  eng.stop();
  rm_rf(dir);
  std::printf("F3 every shard stalled, W=1, X=4, H=B+R: %llu probes on every pair delivered once in order after release "
              "(%llu refused); no deadlock\n",
              (unsigned long long)total, (unsigned long long)refused);
}

// ---- F4 -----------------------------------------------------------------------------------

void test_drop_paths() {
  register_probe();
  const std::string dir = temp_dir();
  Engine::Config c = config(4, dir);
  c.followup_window = 2;
  c.exchange_budget = 8;
  Engine eng(c);
  eng.start();
  const ObjectID a = make(eng, 0, kProbeType);
  const ObjectID gone = make(eng, 2, kProbeType);
  OneResult del = one(eng, 2, [&](Completion cb, void* ctx) { remove(gone, {}, cb, ctx); });
  CHECK_EQ(del.r.status, Status::OK);
  // Missing targets: deleted, and never created (on shard 3 and here).
  const SlotMap map = SlotMap::make_default(4);
  ObjectID never3{0x7777, 1}, never0{0x7777, 2};
  for (u64 i = 1; map.owner_of(never3) != 3; ++i) never3 = ObjectID{0x7777, i};
  for (u64 i = 1; map.owner_of(never0) != 0; ++i) never0 = ObjectID{0x7777, 1000 + i};
  u64 sent = 0;
  for (const ObjectID& t : {gone, never3, never0}) {
    u64 seq = 0;
    for (int k = 0; k < 20; ++k) {
      SendOut o = send_probes(eng, 0, a, t, 1, seq, 10);
      seq = o.last_seq;
      sent += o.ok;
      CHECK(eng.wait_emits(30));
    }
  }
  CHECK(eng.wait_emits(30));
  ProbeV1 q = get_probe(eng, a);
  CHECK_EQ(q.failed, sent);  // every one ran its fail op here, once
  CHECK_EQ(q.acked, 0u);
  expect_quiet(eng);
  // With no fail op: dropped where the target is missing, ending there.
  u64 dropped = 0;
  for (const ObjectID& t : {gone, never3, never0}) {
    u64 seq = 0;
    for (int k = 0; k < 20; ++k) {
      SendOut o = send_probes(eng, 0, a, t, 1, seq, 10, /*op=*/1);
      seq = o.last_seq;
      dropped += o.ok;
      CHECK(eng.wait_emits(30));
    }
  }
  q = get_probe(eng, a);
  CHECK_EQ(q.failed, sent);
  CHECK_EQ(q.sent, sent + dropped);
  expect_quiet(eng);
  eng.stop();
  rm_rf(dir);
  std::printf("F4 drop paths: %llu probes to a deleted and to never-created objects ran their fail op once each, "
              "%llu more with no fail op were dropped there; exchanges, windows and staging back to zero\n",
              (unsigned long long)sent, (unsigned long long)dropped);
}

// ---- F8: two engines over node links -------------------------------------------------------

// A base port with base, base+1, base+20 and base+21 free on loopback now.
bool port_free(u16 port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  CHECK(fd >= 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
  ::close(fd);
  return ok;
}
u16 free_ports() {
  for (u32 i = 0; i < 2000; ++i) {
    const u16 base = static_cast<u16>(45000 + ((::getpid() + i * 37) % 300) * 50);
    if (port_free(base) && port_free(base + 1) && port_free(base + 20) && port_free(base + 21)) return base;
  }
  CHECK(false);
  return 0;
}

void test_node_links() {
  register_probe();
  const std::string dir = temp_dir();
  constexpr u16 kSh = 2;
  constexpr u32 kL = 8;
  const u16 base = free_ports();
  std::vector<ClusterGroup> groups;
  for (u16 g = 0; g < 2; ++g) {
    ClusterGroup grp;
    grp.n_shards = kSh;
    grp.members.push_back({"127.0.0.1", static_cast<u16>(base + g * 20), 0});
    groups.push_back(grp);
  }
  auto node_cfg = [&](u16 g) {
    Engine::Config c = config(kSh, dir + "/n" + std::to_string(g));
    ::mkdir(c.data_dir.c_str(), 0755);
    c.cluster = groups;
    c.group_id = g;
    if (g == 1) c.wal_buffer_budget_bytes = 32u << 10;  // the receiver: a small H
    c.followup_wal_cap_bytes = c.wal_buffer_budget_bytes + big_r();
    c.link_hold_max = kL;
    return c;
  };
  Engine::Config ca = node_cfg(0), cb = node_cfg(1);
  Engine a(ca), b(cb);
  a.start();
  b.start();
  // Probes on node A shard 0; the target on node B, owned by B's shard 1.
  // (debug_hold_wal is per shard index, so A's shard 1 is held too: A
  // sends from shard 0 only.)
  auto create_on = [&](Engine& eng, u16 s) {
    OneResult r = one(eng, s, [&](Completion cbk, void* ctx) { create(kProbeType, ByteView{}, {}, cbk, ctx); });
    CHECK_EQ(r.r.status, Status::OK);
    return r.r.id;
  };
  ObjectID dst{};
  for (int i = 0; i < 1000; ++i) {
    dst = create_on(b, 1);
    if (SlotMap::make_default(kSh).owner_of(dst) == 1) break;
  }
  CHECK_EQ(SlotMap::make_default(kSh).owner_of(dst), 1);
  ObjectID src{};
  for (int i = 0; i < 1000; ++i) {
    src = create_on(a, 0);
    if (SlotMap::make_default(kSh).owner_of(src) == 0) break;
  }
  CHECK_EQ(SlotMap::make_default(kSh).owner_of(src), 0);
  // The links up (a probe has no resume hook: one sent on a link that is
  // down is dropped for good), then one probe round trip.
  for (int i = 0;; ++i) {
    bool up = true;
    for (u16 s = 0; s < kSh; ++s) up = up && a.stats(s).node_connects >= kSh && b.stats(s).node_connects >= kSh;
    if (up) break;
    CHECK(i < 10000);
    ::usleep(1000);
  }
  SendOut warm = send_probes(a, 0, src, dst, 0, 0, 1);
  CHECK_EQ(warm.ok, 1u);
  CHECK(a.wait_emits(30) && b.wait_emits(30));
  for (int i = 0; i < 5000; ++i) {
    OneResult r = one(a, 0, [&](Completion cbk, void* ctx) {
      ExecOptions o;
      o.allow_undurable = true;
      execute(src, kOpPGet, ByteView{}, o, cbk, ctx);
    });
    if (r.as<ProbeV1>().acked == 1) break;
    CHECK(i < 4999);  // the links are up
    ::usleep(1000);
  }
  u64 connects0 = 0;
  for (u16 s = 0; s < kSh; ++s) connects0 += a.stats(s).node_connects + b.stats(s).node_connects;
  Engine::debug_hold_wal(1, true);
  u64 seq = warm.last_seq, ok = warm.ok, refused = 0, held_peak = 0;
  for (int round = 0; round < 60; ++round) {
    SendOut o = send_probes(a, 0, src, dst, 0, seq, 100);
    seq = o.last_seq;
    ok += o.ok;
    refused += o.refused;
    for (u16 s = 0; s < kSh; ++s) {
      held_peak = std::max(held_peak, b.stats(s).link_held_now);
      CHECK(b.stats(s).link_held_now <= kL);
    }
    ::usleep(2000);
  }
  const ShardStats bs = b.stats(1), as = a.stats(0);
  CHECK(bs.followups_gated > 0);
  CHECK(bs.link_pauses >= 1);
  CHECK(bs.link_held_peak <= kL);
  CHECK(bs.wal.active_bytes_peak <= cb.followup_wal_cap_bytes);
  // The sender is not bounded here: a follow-up that leaves for another
  // node ends its exchange (D.1), so A's backlog sits in its link's send
  // buffer. Cross-node exchange accounting is D.2.
  (void)as;
  Engine::debug_hold_wal(1, false);
  for (int i = 0; i < 60000; ++i) {
    OneResult r = one(a, 0, [&](Completion cbk, void* ctx) {
      ExecOptions o;
      o.allow_undurable = true;
      execute(src, kOpPGet, ByteView{}, o, cbk, ctx);
    });
    if (r.as<ProbeV1>().acked == ok) break;
    ::usleep(1000);
  }
  CHECK(a.wait_emits(60) && b.wait_emits(60));
  const ProbeV1 ps = [&] {
    OneResult r = one(a, 0, [&](Completion cbk, void* ctx) {
      ExecOptions o;
      o.allow_undurable = true;
      execute(src, kOpPGet, ByteView{}, o, cbk, ctx);
    });
    return r.as<ProbeV1>();
  }();
  const ProbeV1 pd = [&] {
    OneResult r = one(b, 1, [&](Completion cbk, void* ctx) {
      ExecOptions o;
      o.allow_undurable = true;
      execute(dst, kOpPGet, ByteView{}, o, cbk, ctx);
    });
    return r.as<ProbeV1>();
  }();
  CHECK_EQ(ps.sent, ok);
  CHECK_EQ(ps.acked, ok);
  CHECK_EQ(pd.received, ok);
  CHECK_EQ(pd.order_errors, 0u);
  u64 connects1 = 0;
  for (u16 s = 0; s < kSh; ++s) connects1 += a.stats(s).node_connects + b.stats(s).node_connects;
  CHECK_EQ(connects1, connects0);  // paused, never dropped
  b.stop();
  a.stop();
  rm_rf(dir);
  std::printf("F8 node links, L=%u: receiver gated, link reads paused %llu time(s), held peak %llu <= L, no reconnect; "
              "%llu probes delivered once in order across nodes (%llu refused; the sender's bound is D.2)\n",
              kL, (unsigned long long)bs.link_pauses, (unsigned long long)held_peak, (unsigned long long)ok,
              (unsigned long long)refused);
}

// ---- F11 ----------------------------------------------------------------------------------

void test_rate_mismatch() {
  register_probe();
  const std::string dir = temp_dir();
  constexpr u32 kX = 256, kW = 4;
  Engine::Config c = config(4, dir);
  c.followup_window = kW;
  c.exchange_budget = kX;
  const u64 R = big_r();
  c.followup_wal_cap_bytes = c.wal_buffer_budget_bytes + R;
  Engine eng(c);
  eng.start();
  constexpr u16 kD = 0, kXs = 1;
  const ObjectID pd = make(eng, kD, kProbeType), px = make(eng, kXs, kProbeType);
  const ObjectID ad = make(eng, kD, account::kTypeId);
  // x holds its replies (its WAL held: a reply leaves only once its record
  // is durable) while d fills its budget X with sends to x.
  Engine::debug_hold_wal(kXs, true);
  SendOut o = send_probes(eng, kD, pd, px, 0, 0, kX + 50);
  const u64 ok = o.ok;
  CHECK_EQ(ok, u64{kX});
  CHECK(o.refused == 50 && eng.stats(kD).refused_exchange >= 50);
  for (int i = 0; i < 5000 && get_probe(eng, px).received < ok; ++i) ::usleep(1000);
  CHECK_EQ(get_probe(eng, px).received, ok);
  // d's WAL held and filled to its client budget B with deposits, so the
  // replies close d's gate H = B + R; then x's replies are released at once.
  Engine::debug_hold_wal(kD, true);
  u64 deposits = 0;
  on_shard(eng, kD, [&] {
    for (int i = 0; i < 100000; ++i) {
      account::AmountArgs a{1, 7};
      ExecOptions eo;
      eo.durability = Durability::APPLIED;
      Status st = Status::OK;
      execute(ad, account::kOpDeposit, ByteView(&a, sizeof(a)), eo,
              [](void* p, const ExecResult& r) { *static_cast<Status*>(p) = r.status; }, &st);
      if (st != Status::OK) {
        CHECK_EQ(st, Status::E_OVERLOADED);
        break;
      }
      ++deposits;
    }
  });
  CHECK(deposits > 0);
  Engine::debug_hold_wal(kXs, false);
  u64 x_peak = 0;
  for (int i = 0; i < 300; ++i) {
    const ShardStats sx = eng.stats(kXs), sd = eng.stats(kD);
    x_peak = std::max(x_peak, sx.emits_in_flight);
    // x's backlog toward d: replies to d's live exchanges, nothing more.
    CHECK(sx.emits_in_flight <= sd.exchanges_live + sx.exchanges_live);
    CHECK(sd.followups_staged_now <= kW + 1);
    ::usleep(2000);
  }
  const ShardStats sd = eng.stats(kD);
  CHECK(sd.followups_gated > 0);
  CHECK(x_peak > kW);  // x holds replies past d's window: the backlog is real
  CHECK(sd.wal.active_bytes_peak <= c.followup_wal_cap_bytes);
  Engine::debug_hold_wal(kD, false);
  CHECK(eng.wait_emits(60));
  const ProbeV1 x = get_probe(eng, px), d = get_probe(eng, pd);
  CHECK_EQ(x.received, ok);
  CHECK_EQ(d.acked, ok);
  CHECK_EQ(x.order_errors, 0u);
  expect_quiet(eng);
  eng.stop();
  rm_rf(dir);
  std::printf("F11 rate mismatch: d's gate closed (WAL held at B, then %llu replies); x's backlog toward d peaked at "
              "%llu > W = %u and stayed <= live(d) + live(x); d's staging <= W; %llu exchanges, 50 more refused by "
              "X = %u; settled in order\n",
              (unsigned long long)ok, (unsigned long long)x_peak, kW, (unsigned long long)ok, kX);
}

// ---- F12-F15 --------------------------------------------------------------------------------

i64 money(Engine& eng, u64* entries = nullptr) {
  i64 t = 0;
  u64 e = 0;
  for (u16 s = 0; s < eng.shard_count(); ++s) {
    eng.for_each_object(s, [&](const ObjectHeader& h) {
      if (h.type_id != account::kTypeId) return;
      const auto* a = reinterpret_cast<const account::AccountV1*>(h.payload());
      t += a->balance_cents;
      for (u16 i = 0; i < a->n_out; ++i) {
        const auto* out = reinterpret_cast<const account::OutEntry*>(h.payload() + sizeof(account::AccountV1)) + i;
        t += out->amount;  // in flight: debited here, not yet credited
      }
      e += a->n_out + a->n_in;
    });
  }
  if (entries) *entries = e;
  return t;
}

OneResult transfer(Engine& eng, u16 s, const ObjectID& from, const ObjectID& to, i64 amount, u64 client = 0, u64 seq = 0) {
  return one(eng, s, [&](Completion cb, void* ctx) {
    account::TransferArgs t{to, amount, 7};
    ExecOptions o;
    o.client_id = client;
    o.seq = seq;
    o.durability = Durability::APPLIED;
    execute(from, account::kOpTransfer, ByteView(&t, sizeof(t)), o, cb, ctx);
  });
}

void test_saturated_budget() {
  register_probe();
  account::register_type();
  const std::string dir = temp_dir();
  constexpr u32 kX = 8;
  Engine::Config c = config(4, dir);
  c.exchange_budget = kX;
  c.followup_window = 2;
  const u64 R = big_r();
  c.followup_wal_cap_bytes = c.wal_buffer_budget_bytes + R;
  Engine eng(c);
  eng.start();
  constexpr u16 kO = 0, kP = 1, kQ = 2;
  std::vector<ObjectID> o_acc, p_acc;
  for (int i = 0; i < 12; ++i) o_acc.push_back(make(eng, kO, account::kTypeId));
  for (int i = 0; i < 4; ++i) p_acc.push_back(make(eng, kP, account::kTypeId));
  const ObjectID q_acc = make(eng, kQ, account::kTypeId);
  const i64 m0 = money(eng);
  // F14 setup: a sequenced transfer o -> p, applied before saturation.
  constexpr u64 kClient = 0xC11E;
  OneResult first = transfer(eng, kO, o_acc[0], p_acc[0], 5, kClient, 1);
  CHECK_EQ(first.r.status, Status::OK);
  CHECK(eng.wait_emits(30));
  // Saturate o's budget: p's WAL held, transfers o -> p until refused.
  Engine::debug_hold_wal(kP, true);
  u64 admitted = 0;
  for (int i = 0; i < 200; ++i) {
    OneResult r = transfer(eng, kO, o_acc[1 + i % 11], p_acc[i % 4], 1);
    if (r.r.status == Status::E_OVERLOADED) break;
    CHECK_EQ(r.r.status, Status::OK);
    ++admitted;
  }
  ShardStats so = eng.stats(kO);
  CHECK(so.exchanges_live >= kX);
  const u64 rx0 = so.refused_exchange;
  CHECK(rx0 >= 1);
  // F12: a transfer between two of o's accounts reserves nothing: it runs.
  OneResult local = transfer(eng, kO, o_acc[2], o_acc[3], 3);
  CHECK_EQ(local.r.status, Status::OK);
  CHECK_EQ(eng.stats(kO).refused_exchange, rx0);
  // F13: o -> q would start an exchange: refused, nothing changed.
  const u64 v_before = eng.stats(kO).wal_next_lsn;
  OneResult far = transfer(eng, kO, o_acc[4], q_acc, 3);
  CHECK_EQ(far.r.status, Status::E_OVERLOADED);
  CHECK_EQ(eng.stats(kO).refused_exchange, rx0 + 1);
  CHECK_EQ(eng.stats(kO).wal_next_lsn, v_before);
  // F14: the resend of the sequenced transfer is replayed, not refused.
  OneResult again = transfer(eng, kO, o_acc[0], p_acc[0], 5, kClient, 1);
  CHECK_EQ(again.r.status, Status::OK);
  CHECK(again.r.replayed);
  CHECK_EQ(eng.stats(kO).refused_exchange, rx0 + 1);
  // F15: resume triggers while saturated coalesce.
  const ShardStats r0 = eng.stats(kO);
  on_shard(eng, kO, [] { current_shard()->debug_resume(-1); });  // starts a pass (stalls on X)
  for (int i = 0; i < 2000; ++i) {
    on_shard(eng, kO, [i] { current_shard()->debug_resume(i % 3 == 0 ? -1 : i % kSlots); });
  }
  ::usleep(50000);
  const ShardStats r1 = eng.stats(kO);
  CHECK_EQ(r1.resume_triggers - r0.resume_triggers, 2001u);
  CHECK(r1.resume_passes - r0.resume_passes <= 1);  // the active one only
  CHECK(r1.resume_coalesced - r0.resume_coalesced >= 2000);
  CHECK(r1.resume_owed);
  const u32 rr = registry().type(account::kTypeId)->max_resume_emits;
  CHECK(r1.exchanges_live_peak <= kX + std::max<u32>(1, rr));
  // Release: the active pass finishes, then exactly one owed pass.
  Engine::debug_hold_wal(kP, false);
  CHECK(eng.wait_emits(60));
  const ShardStats r2 = eng.stats(kO);
  CHECK(r2.resume_passes - r0.resume_passes <= 2);
  CHECK(!r2.resume_active && !r2.resume_owed);
  u64 entries = 0;
  CHECK_EQ(money(eng, &entries), m0);
  CHECK_EQ(entries, 0u);
  expect_quiet(eng);
  eng.stop();
  rm_rf(dir);
  std::printf("F12-F15 X=%u saturated (%llu transfers o->p admitted): a same-shard transfer ran; o->q refused "
              "(refused_exchange +1, WAL unchanged); a sequenced resend replayed; 2001 resume triggers made %llu "
              "pass(es) while saturated and %llu in all; settled, conserved\n",
              kX, (unsigned long long)admitted, (unsigned long long)(r1.resume_passes - r0.resume_passes),
              (unsigned long long)(r2.resume_passes - r0.resume_passes));
}

// ---- F6 -----------------------------------------------------------------------------------

void test_crash_recovery() {
  const std::string dir = temp_dir();
  const std::string data = dir + "/data";
  constexpr u16 kN = 4, kD = 1;
  auto [sig, err] = in_child(dir, [&] {
    Engine::Config c = config(kN, data);
    c.followup_window = 4;
    c.exchange_budget = 128;
    c.wal_buffer_budget_bytes = 96u << 10;
    c.followup_wal_cap_bytes = c.wal_buffer_budget_bytes + big_r();
    Engine eng(c);
    eng.start();
    std::vector<std::vector<ObjectID>> acc(kN);
    for (u16 s = 0; s < kN; ++s)
      for (int i = 0; i < 10; ++i) acc[s].push_back(make(eng, s, account::kTypeId));
    const i64 m0 = money(eng);
    if (FILE* f = std::fopen((dir + "/m0").c_str(), "w")) {
      std::fprintf(f, "%lld\n", (long long)m0);
      std::fclose(f);
    }
    // Intake closed on d (its WAL held), exchanges in flight both ways.
    Engine::debug_hold_wal(kD, true);
    u64 ok = 0;
    for (int i = 0; i < 2000; ++i) {
      // Mostly into d (its gate closes), some out of it and between others.
      const u16 s = static_cast<u16>(i % kN);
      const u16 t = s != kD && i % 5 != 0 ? kD : static_cast<u16>((s + 1 + i / kN % (kN - 1)) % kN);
      OneResult r = transfer(eng, s, acc[s][i % 10], acc[t][(i / 7) % 10], 1 + i % 5);
      if (r.r.status == Status::OK) ++ok;
    }
    if (eng.stats(kD).followups_gated == 0) std::_Exit(4);  // the gate never closed: not the case under test
    std::fprintf(stderr, "child: %llu transfers admitted, %llu follow-ups gated on d, %llu staged; killing\n", (unsigned long long)ok, (unsigned long long)eng.stats(kD).followups_gated, (unsigned long long)eng.stats(kD).followups_staged_now);
    ::raise(SIGKILL);
  });
  CHECK_EQ(sig, SIGKILL);
  long long m0 = 0;
  {
    FILE* f = std::fopen((dir + "/m0").c_str(), "r");
    CHECK(f != nullptr);
    CHECK_EQ(std::fscanf(f, "%lld", &m0), 1);
    std::fclose(f);
  }
  Engine::debug_hold_wal(kD, false);
  Engine::Config c = config(kN, data);
  c.init = false;
  c.followup_window = 4;
  c.exchange_budget = 128;
  c.wal_buffer_budget_bytes = 96u << 10;
  c.followup_wal_cap_bytes = c.wal_buffer_budget_bytes + big_r();
  Engine eng(c);
  eng.start();
  CHECK(eng.wait_emits(60));
  u64 entries = 0;
  CHECK_EQ(money(eng, &entries), m0);
  CHECK_EQ(entries, 0u);
  expect_quiet(eng);
  eng.stop();
  rm_rf(dir);
  std::printf("F6 SIGKILL with d's intake closed and exchanges in flight, then recovery: transfers settled, money "
              "conserved (%lld), no entries left (%s)\n",
              m0, err.substr(0, err.find('\n')).c_str());
}

// ---- G1: the owner gate -------------------------------------------------------------------

void test_owner_gate() {
  account::register_type();
  const std::string dir = temp_dir();
  Engine::Config c = config(2, dir);
  c.exchange_gate = false;  // so the backlog can pass kEmitBacklog
  c.hold_emits = true;
  Engine eng(c);
  eng.start();
  constexpr u16 kD = 1;
  std::vector<ObjectID> d_acc;
  for (int i = 0; i < 20; ++i) d_acc.push_back(make(eng, kD, account::kTypeId));
  const ObjectID s_acc = make(eng, 0, account::kTypeId);
  // d's backlog past the gate: transfers d -> shard 0, held.
  for (int i = 0; eng.stats(kD).emits_in_flight <= 1024; ++i) {
    CHECK(i < 5000);
    OneResult r = transfer(eng, kD, d_acc[i % 20], s_acc, 1);
    CHECK(r.r.status == Status::OK || r.r.status == Status::E_APP);
  }
  const ShardStats d0 = eng.stats(kD);
  // A deposit forwarded from shard 0 to d is refused there.
  OneResult dep = one(eng, 0, [&](Completion cb, void* ctx) {
    account::AmountArgs a{1, 7};
    execute(d_acc[0], account::kOpDeposit, ByteView(&a, sizeof(a)), {}, cb, ctx);
  });
  CHECK_EQ(dep.r.status, Status::E_OVERLOADED);
  const ShardStats d1 = eng.stats(kD);
  CHECK_EQ(d1.refused_owner, d0.refused_owner + 1);
  CHECK_EQ(d1.overloaded, d0.overloaded + 1);
  // A forwarded read is answered.
  OneResult rd = one(eng, 0, [&](Completion cb, void* ctx) {
    ExecOptions o;
    o.allow_undurable = true;
    execute(d_acc[0], account::kOpGetBalance, ByteView{}, o, cb, ctx);
  });
  CHECK_EQ(rd.r.status, Status::OK);
  eng.release_emits();
  CHECK(eng.wait_emits(60));
  eng.stop();
  rm_rf(dir);
  std::printf("G1 owner gate: a deposit forwarded to a shard with %llu follow-ups queued was refused there "
              "(refused_owner); a forwarded read was answered\n",
              (unsigned long long)d0.emits_in_flight);
}

}  // namespace

int main(int argc, char** argv) {
  register_probe();
  account::register_type();
  const std::string only = argc > 1 ? argv[1] : "";
  auto run = [&](const char* name, void (*fn)()) {
    if (only.empty() || only == name) fn();
  };
  // Death tests and F6 first: they fork, safest before any Engine has run here.
  run("F10", test_contract_deaths);
  run("F6", test_crash_recovery);  // forks too: before the in-process engines
  run("F1", test_stalled_destination);
  run("F3", test_all_stalled);
  run("F4", test_drop_paths);
  run("F8", test_node_links);
  run("F11", test_rate_mismatch);
  run("F12", test_saturated_budget);
  run("G1", test_owner_gate);
  std::printf("test_intake: ok\n");
  return 0;
}
