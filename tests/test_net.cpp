// Socket front end and owner-aware client (docs/ROUTING.md): topology, creates
// that stay on their entry shard (mint_local), requests sent to the owner run
// there, requests sent elsewhere are forwarded and say so, durable responses,
// and a client that disconnects with requests in flight.
#include <stdlib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <latch>
#include <cstring>
#include <map>
#include <string>

#include "harness.h"
#include "snc/net.h"
#include "snc/snc.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::account;

namespace {

struct Got {
  wire::RespHeader h;
  std::vector<u8> body;
};

// Waits for `n` responses and returns them by req_id.
std::map<u64, Got> collect(net::Client& c, std::size_t n) {
  std::map<u64, Got> out;
  while (out.size() < n) {
    int r = c.poll(
        [](void* ctx, const wire::RespHeader& h, ByteView b) {
          auto& m = *static_cast<std::map<u64, Got>*>(ctx);
          m[h.req_id] = Got{h, std::vector<u8>(b.data, b.data + b.len)};
        },
        &out, 100);
    CHECK(r >= 0);
  }
  return out;
}

CreateArgs account_args(u64 i) {
  CreateArgs a{};
  a.balance_cents = 1000;
  a.created_ns = i;
  a.email_len = static_cast<u8>(std::snprintf(a.email, sizeof(a.email), "n%llu@x", (unsigned long long)i));
  a.name_len = 1;
  a.name[0] = 'x';
  return a;
}

void run(const std::string& data_dir, u16 port) {
  const u16 n = 3;
  Engine::Config cfg;
  cfg.runtime.n_shards = n;
  cfg.runtime.pin = false;
  cfg.expected_objects = 10000;
  cfg.mint_local = true;
  cfg.data_dir = data_dir;
  cfg.init = true;
  net::NetServer::Config ncfg;
  ncfg.base_port = port;
  net::NetServer server(ncfg, n);
  cfg.app_poll = net::NetServer::poll_hook;
  cfg.app_user = &server;
  Engine eng(cfg);
  eng.start();

  net::Client c;
  CHECK(c.connect("127.0.0.1", port));
  CHECK_EQ(c.shard_count(), n);
  CHECK_EQ(c.slot_map().owner[7], SlotMap::make_default(n).owner[7]);

  // Creates sent to every shard run on that shard and mint IDs it owns.
  const u64 kAccounts = 600;
  for (u64 i = 0; i < kAccounts; ++i) {
    CreateArgs a = account_args(i);
    wire::ReqHeader q{};
    q.kind = wire::Kind::CREATE;
    q.op = kTypeId;
    q.req_id = i;
    c.send(static_cast<u16>(i % n), q, ByteView(&a, sizeof(a)));
  }
  auto created = collect(c, kAccounts);
  std::vector<ObjectID> ids(kAccounts);
  for (auto& [i, g] : created) {
    CHECK_EQ(g.h.status, u16(Status::OK));
    CHECK_EQ(g.h.owner, i % n);
    CHECK_EQ(c.owner_of(g.h.id), i % n);
    CHECK(!(g.h.flags & wire::kRespForwarded));
    CHECK(g.h.flags & wire::kRespDurable);  // in memory every state reports durable
    ids[i] = g.h.id;
  }
  for (u16 s = 0; s < n; ++s) CHECK_EQ(eng.stats(s).ops_remote, 0u);

  // A CREATE naming a target places the new object in the target's slot
  // (create_near), wherever it is sent.
  u64 near_forwarded = 0;
  for (u64 i = 0; i < 8; ++i) {
    near_forwarded += static_cast<u16>((i + 1) % n) != c.owner_of(ids[i]);
    CreateArgs a = account_args(kAccounts + i);
    wire::ReqHeader q{};
    q.kind = wire::Kind::CREATE;
    q.op = kTypeId;
    q.req_id = i;
    q.target = ids[i];
    c.send(static_cast<u16>((i + 1) % n), q, ByteView(&a, sizeof(a)));
  }
  for (auto& [i, g] : collect(c, 8)) {
    CHECK_EQ(g.h.status, u16(Status::OK));
    CHECK_EQ(slot_of(g.h.id), slot_of(ids[i]));
    CHECK_EQ(g.h.owner, c.owner_of(ids[i]));
  }

  // Deposits: even i to the owner, odd i to the next shard (forwarded).
  for (u64 i = 0; i < kAccounts; ++i) {
    u16 owner = c.owner_of(ids[i]);
    wire::ReqHeader q{};
    q.kind = wire::Kind::EXEC;
    q.op = kOpDeposit;
    q.req_id = i;
    q.target = ids[i];
    AmountArgs d{static_cast<i64>(i + 1), 1};
    c.send(i % 2 ? static_cast<u16>((owner + 1) % n) : owner, q, ByteView(&d, sizeof(d)));
  }
  auto dep = collect(c, kAccounts);
  for (auto& [i, g] : dep) {
    CHECK_EQ(g.h.status, u16(Status::OK));
    CHECK_EQ(g.h.version, 2u);
    CHECK_EQ(bool(g.h.flags & wire::kRespForwarded), i % 2 == 1);
    CHECK_EQ(g.h.owner, c.owner_of(ids[i]));
  }
  CHECK_EQ(c.forwarded(), kAccounts / 2 + near_forwarded);

  // Reads return the balance through either path.
  for (u64 i = 0; i < kAccounts; ++i) {
    wire::ReqHeader q{};
    q.kind = wire::Kind::EXEC;
    q.op = kOpGetBalance;
    q.req_id = i;
    q.target = ids[i];
    c.send(static_cast<u16>(i % n), q, {});
  }
  auto bal = collect(c, kAccounts);
  for (auto& [i, g] : bal) {
    CHECK_EQ(g.h.status, u16(Status::OK));
    CHECK_EQ(g.body.size(), sizeof(BalanceResult));
    BalanceResult r;
    std::memcpy(&r, g.body.data(), sizeof(r));
    CHECK_EQ(r.balance_cents, static_cast<i64>(1001 + i));
    CHECK(g.h.flags & wire::kRespDurable);  // in memory every state reports durable
  }

  // Unknown target and unknown op.
  {
    wire::ReqHeader q{};
    q.kind = wire::Kind::EXEC;
    q.op = kOpGetBalance;
    q.req_id = 1;
    q.target = ObjectID::make(999999, 1);
    c.send(0, q, {});
    q.req_id = 2;
    q.target = ids[0];
    q.op = 4242;
    c.send(c.owner_of(ids[0]), q, {});
    auto e = collect(c, 2);
    CHECK_EQ(e[1].h.status, u16(Status::E_NOT_FOUND));
    CHECK_EQ(e[2].h.status, u16(Status::E_UNKNOWN_OP));
  }

  // INDEX (docs/EXPERIMENTAL_BASELINE.md X1): index_lookup on the wire. Ten
  // accounts share an email, spread over every shard; any shard answers
  // for the whole node, with the IDs the engine's own lookup returns.
  {
    std::vector<ObjectID> shared;
    for (u64 i = 0; i < 10; ++i) {
      CreateArgs a = account_args(5000 + i);
      a.email_len = static_cast<u8>(std::snprintf(a.email, sizeof(a.email), "shared@x"));
      wire::ReqHeader q{};
      q.kind = wire::Kind::CREATE;
      q.op = kTypeId;
      q.req_id = 100 + i;
      c.send(static_cast<u16>(i % n), q, ByteView(&a, sizeof(a)));
    }
    for (auto& [i, g] : collect(c, 10)) {
      CHECK_EQ(g.h.status, u16(Status::OK));
      shared.push_back(g.h.id);
    }
    auto query = [&](u64 req_id, u32 index_id, const std::string& key, u32 max, u8 flags = 0,
                     u32 body_len = ~0u, u32 reserved = 0) {
      std::vector<u8> body(sizeof(wire::IndexQueryBody) + key.size());
      wire::IndexQueryBody b{max, reserved};
      std::memcpy(body.data(), &b, sizeof(b));
      std::memcpy(body.data() + sizeof(b), key.data(), key.size());
      wire::ReqHeader q{};
      q.kind = wire::Kind::INDEX;
      q.op = index_id;
      q.flags = flags;
      q.req_id = req_id;
      c.send(static_cast<u16>(req_id % n), q,
             ByteView(body.data(), body_len == ~0u ? static_cast<u32>(body.size()) : body_len));
    };
    auto ids_of = [](const Got& g) {
      std::vector<ObjectID> v(g.body.size() / sizeof(ObjectID));
      std::memcpy(v.data(), g.body.data(), v.size() * sizeof(ObjectID));
      std::sort(v.begin(), v.end());
      return v;
    };
    std::sort(shared.begin(), shared.end());
    // The same answer from every shard; matches the in-process lookup.
    for (u64 r = 0; r < n; ++r) query(200 + r, kEmailIndex, "shared@x", 100);
    auto got = collect(c, n);
    for (u64 r = 0; r < n; ++r) {
      CHECK_EQ(got[200 + r].h.status, u16(Status::OK));
      CHECK_EQ(got[200 + r].body.size(), 10 * sizeof(ObjectID));
      CHECK(ids_of(got[200 + r]) == shared);
    }
    std::vector<ObjectID> local;
    {
      std::latch done(1);
      struct L {
        std::vector<ObjectID>* out;
        std::latch* done;
      } l{&local, &done};
      eng.runtime().submit(0, [&] {
        index_lookup(kEmailIndex, ByteView("shared@x", 8), 100,
                     [](void* ctx, Status st, const ObjectID* v, u32 k) {
                       auto* x = static_cast<L*>(ctx);
                       CHECK_EQ(st, Status::OK);
                       x->out->assign(v, v + k);
                       x->done->count_down();
                     },
                     &l);
      });
      done.wait();
    }
    std::sort(local.begin(), local.end());
    CHECK(local == shared);
    // max_results caps the answer; a key nobody has answers OK and empty;
    // a single-owner email finds its account.
    query(210, kEmailIndex, "shared@x", 3);
    query(211, kEmailIndex, "nobody@x", 10);
    query(212, kEmailIndex, "n7@x", 10);
    got = collect(c, 3);
    CHECK_EQ(got[210].h.status, u16(Status::OK));
    CHECK_EQ(got[210].body.size(), 3 * sizeof(ObjectID));
    for (const ObjectID& id : ids_of(got[210])) CHECK(std::binary_search(shared.begin(), shared.end(), id));
    CHECK_EQ(got[211].h.status, u16(Status::OK));
    CHECK_EQ(got[211].body.size(), 0u);
    CHECK_EQ(got[212].h.status, u16(Status::OK));
    CHECK(ids_of(got[212]) == std::vector<ObjectID>{ids[7]});
    // A key longer than a message's inline bytes (kMsgInlineBytes) goes to
    // the other shards in an allocated buffer: six accounts with a 40-byte
    // email, found from every shard.
    {
      const std::string longkey = "a-forty-byte-email-address-0123@long.xyz";
      CHECK_EQ(longkey.size(), 40u);
      std::vector<ObjectID> lng;
      for (u64 i = 0; i < 6; ++i) {
        CreateArgs a = account_args(6000 + i);
        a.email_len = static_cast<u8>(longkey.size());
        std::memcpy(a.email, longkey.data(), longkey.size());
        wire::ReqHeader q{};
        q.kind = wire::Kind::CREATE;
        q.op = kTypeId;
        q.req_id = 300 + i;
        c.send(static_cast<u16>(i % n), q, ByteView(&a, sizeof(a)));
      }
      for (auto& [i, g] : collect(c, 6)) {
        CHECK_EQ(g.h.status, u16(Status::OK));
        lng.push_back(g.h.id);
      }
      std::sort(lng.begin(), lng.end());
      for (u64 r = 0; r < n; ++r) query(310 + r, kEmailIndex, longkey, 100);
      auto lg = collect(c, n);
      for (u64 r = 0; r < n; ++r) {
        CHECK_EQ(lg[310 + r].h.status, u16(Status::OK));
        CHECK(ids_of(lg[310 + r]) == lng);
      }
    }
    // Refused cleanly, the connection kept: an unknown index, a key longer
    // than kMaxIndexKey, max_results 0 or above kMaxIndexResults, a body
    // shorter than IndexQueryBody, the sequenced flag, a non-zero reserved.
    query(220, 999, "shared@x", 10);
    query(221, kEmailIndex, std::string(kMaxIndexKey + 1, 'k'), 10);
    query(222, kEmailIndex, "shared@x", 0);
    query(223, kEmailIndex, "shared@x", wire::kMaxIndexResults + 1);
    query(224, kEmailIndex, "shared@x", 10, 0, 4);
    query(225, kEmailIndex, "shared@x", 10, wire::kReqSequenced);
    query(226, kEmailIndex, "shared@x", 10, 0, ~0u, 1);
    got = collect(c, 7);
    for (u64 r = 220; r <= 226; ++r) {
      CHECK_EQ(got[r].h.status, u16(Status::E_BAD_ARGS));
      CHECK_EQ(got[r].body.size(), 0u);
    }
    // A kind this server does not know (as INDEX is to a server from before
    // it) is answered E_BAD_ARGS, and the connection stays usable: what an
    // older server does with INDEX.
    {
      wire::ReqHeader q{};
      q.kind = static_cast<wire::Kind>(7);
      q.req_id = 230;
      c.send(0, q, {});
      q.kind = wire::Kind::EXEC;
      q.op = kOpGetBalance;
      q.req_id = 231;
      q.target = ids[3];
      c.send(c.owner_of(ids[3]), q, {});
      got = collect(c, 2);
      CHECK_EQ(got[230].h.status, u16(Status::E_BAD_ARGS));
      CHECK_EQ(got[231].h.status, u16(Status::OK));
    }
  }

  // A client that disconnects with requests in flight, and one that sends a
  // malformed frame, must not disturb the server.
  {
    auto gone = std::make_unique<net::Client>();
    CHECK(gone->connect("127.0.0.1", port));
    for (u64 i = 0; i < 2000; ++i) {
      wire::ReqHeader q{};
      q.kind = wire::Kind::EXEC;
      q.op = kOpDeposit;
      q.req_id = i;
      q.target = ids[i % kAccounts];
      AmountArgs d{1, 1};
      gone->send(static_cast<u16>(i % n), q, ByteView(&d, sizeof(d)));
    }
    gone->poll([](void*, const wire::RespHeader&, ByteView) {}, nullptr, 0);
    gone.reset();

    // A frame shorter than a request header: the server drops the connection.
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u16>(port + 1));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    u32 junk[4] = {8, 0, 0, 0};
    CHECK(::send(fd, junk, sizeof(junk), 0) == sizeof(junk));
    char buf[64];
    CHECK(::recv(fd, buf, sizeof(buf), 0) == 0);  // closed by the server
    ::close(fd);
  }

  // The original client still works.
  {
    wire::ReqHeader q{};
    q.kind = wire::Kind::EXEC;
    q.op = kOpGetBalance;
    q.req_id = 7;
    q.target = ids[7];
    c.send(c.owner_of(ids[7]), q, {});
    auto g = collect(c, 1);
    CHECK_EQ(g[7].h.status, u16(Status::OK));
  }

  eng.stop();
}

}  // namespace

int main() {
  register_type();
  u16 port = test::free_ports(20000, 26000, 20);
  run("", port);
  char tmpl[] = "/tmp/snc-test-net-XXXXXX";
  CHECK(mkdtemp(tmpl) != nullptr);
  run(tmpl, static_cast<u16>(port + 10));
  std::string cmd = std::string("rm -rf ") + tmpl;
  CHECK(std::system(cmd.c_str()) == 0);
  std::puts("test_net: OK");
  return 0;
}
