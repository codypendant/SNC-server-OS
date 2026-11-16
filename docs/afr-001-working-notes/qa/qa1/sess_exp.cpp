// QA1 session-dedup experiments (P5, P6, P7). Usage: sess_exp <dir>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <string>

#include "harness.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::account;
using namespace snc::test;

// Interposed wall clock: libsnc_core.a is linked into this executable, so its
// calls to time() resolve here (P7).
static long g_fake = 0;
extern "C" time_t time(time_t* t) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  time_t v = ts.tv_sec + g_fake;
  if (t) *t = v;
  return v;
}

static Engine* E;

static ObjectID mk(i64 bal, const char* email) {
  CreateArgs a{};
  a.balance_cents = bal;
  a.created_ns = 1;
  a.email_len = (u8)std::strlen(email);
  std::memcpy(a.email, email, a.email_len);
  a.name_len = 1;
  a.name[0] = 'x';
  OneResult r = one(*E, 0, [&](Completion c, void* ctx) {
    create(kTypeId, ByteView{&a, sizeof(a)}, ExecOptions{}, c, ctx);
  });
  return r.r.id;
}

static i64 bal(ObjectID id) {
  OneResult r = one(*E, 0, [&](Completion c, void* ctx) { execute(id, kOpGetBalance, {}, {}, c, ctx); });
  if (r.r.status != Status::OK) return -999999;
  return r.as<BalanceResult>().balance_cents;
}

static OneResult op(ObjectID id, OpID o, i64 amt, u64 cid, u64 seq, u64 ev = 0) {
  AmountArgs a{amt, 1};
  ExecOptions x;
  x.client_id = cid;
  x.seq = seq;
  x.expected_version = ev;
  return one(*E, 0, [&](Completion c, void* ctx) { execute(id, o, ByteView{&a, sizeof(a)}, x, c, ctx); });
}

static void show(const char* what, const OneResult& r, ObjectID asked) {
  std::printf("%-58s status=%-18s replayed=%d id_is_target=%d result_len=%zu ver=%llu\n", what,
              status_name(r.r.status), r.r.replayed, r.r.id == asked, r.bytes.size(),
              (unsigned long long)r.r.version);
}

int main(int argc, char** argv) {
  register_type();
  Engine::Config c;
  c.runtime.n_shards = 1;
  c.runtime.ring_slots = 64;
  c.expected_objects = 1000;
  c.data_dir = argv[1];
  c.init = true;
  Engine eng(c);
  eng.start();
  E = &eng;
  ObjectID A = mk(1000, "a@x"), B = mk(1000, "b@x"), Z = mk(5, "z@x"), Y = mk(0, "y@x");
  std::printf("session_ttl_s=%u\n", c.session_ttl_s);

  std::printf("--- P5: same (client,seq), different target/op/args\n");
  show("c7 s1 deposit A +100", op(A, kOpDeposit, 100, 7, 1), A);
  show("c7 s1 deposit B +500 (different target+args)", op(B, kOpDeposit, 500, 7, 1), B);
  show("c7 s1 withdraw B 1 (different op)", op(B, kOpWithdraw, 1, 7, 1), B);
  std::printf("balances A=%lld (expect 1100) B=%lld (unchanged would be 1000)\n", (long long)bal(A), (long long)bal(B));
  {
    CreateArgs a{};
    a.balance_cents = 1;
    a.email_len = 3;
    std::memcpy(a.email, "n@x", 3);
    ExecOptions x;
    x.client_id = 7;
    x.seq = 1;
    OneResult r = one(eng, 0, [&](Completion cc, void* ctx) { create(kTypeId, ByteView{&a, sizeof(a)}, x, cc, ctx); });
    show("c7 s1 CREATE (same seq)", r, A);
  }

  std::printf("--- P6: failed sequenced mutations are not recorded\n");
  show("c7 s2 withdraw Z 1000 (insufficient)", op(Z, kOpWithdraw, 1000, 7, 2), Z);
  show("c7 s2 resend: deposit Z +1", op(Z, kOpDeposit, 1, 7, 2), Z);
  std::printf("Z=%lld (6 = resend executed)\n", (long long)bal(Z));
  show("c7 s3 deposit A +1 expected_version=999", op(A, kOpDeposit, 1, 7, 3, 999), A);
  show("c7 s3 resend without version", op(A, kOpDeposit, 1, 7, 3), A);
  ObjectID ghost = ObjectID::make(424242, 1);
  show("c7 s4 deposit on missing object", op(ghost, kOpDeposit, 1, 7, 4), ghost);
  show("c7 s4 resend on A", op(A, kOpDeposit, 1, 7, 4), A);
  show("c7 s5 withdraw Z 1000 (fails)", op(Z, kOpWithdraw, 1000, 7, 5), Z);
  show("c7 s6 deposit Z +1", op(Z, kOpDeposit, 1, 7, 6), Z);
  show("c7 s5 resend (never applied)", op(Z, kOpWithdraw, 1, 7, 5), Z);
  {
    ExecOptions x;
    x.client_id = 7;
    x.seq = 7;
    OneResult r1 = one(eng, 0, [&](Completion cc, void* ctx) { remove(Y, x, cc, ctx); });
    show("c7 s7 delete Y (balance 0)", r1, Y);
    OneResult r2 = one(eng, 0, [&](Completion cc, void* ctx) { remove(Y, x, cc, ctx); });
    show("c7 s7 delete Y resend", r2, Y);
    show("c7 s7 deposit A (seq 7 not consumed by delete?)", op(A, kOpDeposit, 1, 7, 7), A);
  }

  std::printf("--- P7: wall-clock step and expiry at checkpoint\n");
  show("c9 s1 deposit A +1000", op(A, kOpDeposit, 1000, 9, 1), A);
  i64 a0 = bal(A);
  eng.checkpoint();
  show("c9 s1 resend after checkpoint, clock normal", op(A, kOpDeposit, 1000, 9, 1), A);
  std::printf("A=%lld (no change expected, was %lld)\n", (long long)bal(A), (long long)a0);
  g_fake = 3700;  // wall clock steps forward ~1h
  show("c9 s1 resend after clock step, before checkpoint", op(A, kOpDeposit, 1000, 9, 1), A);
  CheckpointReport cr = eng.checkpoint();
  std::printf("checkpoint seq=%llu\n", (unsigned long long)cr.seq);
  show("c9 s1 resend after clock step + checkpoint", op(A, kOpDeposit, 1000, 9, 1), A);
  std::printf("A=%lld (was %lld: +1000 means applied twice)\n", (long long)bal(A), (long long)a0);
  eng.stop();
  return 0;
}
