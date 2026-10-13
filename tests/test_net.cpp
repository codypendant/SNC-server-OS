// Socket front end and owner-aware client (docs/ROUTING.md): topology, creates
// that stay on their entry shard (mint_local), requests sent to the owner run
// there, requests sent elsewhere are forwarded and say so, durable responses,
// and a client that disconnects with requests in flight.
#include <stdlib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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
