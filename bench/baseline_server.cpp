// Thread-pool baseline behind the same socket front end as snc_server, for
// the routing comparison (docs/ROUTING.md). Worker i listens on port + i and
// runs every request it receives itself, against the shared striped map, so
// which worker a client picks changes nothing but cache and lock locality.
// Durable mode holds each response until its WAL record (or, for a read, the
// record of the state it saw) is durable, as baseline_bench does.
//
// Usage: baseline_server [--threads N] [--cpus 0,1,...] [--host H] [--port P]
//                        [--objects N] [--data-dir DIR] [--logs K] [--group-commit-us N]
//                        [--no-prefetch]
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>

#include <algorithm>
#include <cstdio>
#include <deque>

#include "baseline_core.h"
#include "snc/net.h"

using namespace snc;
using namespace snc::account;
using namespace snc::baseline;

namespace {

struct Server {
  u16 threads = 0;
  Map map;
  Logs logs;
  std::atomic<bool> stop{false};
};

struct Pending {
  net::ConnSet::Conn* conn;
  wire::RespHeader h;
  BalanceResult result;
  bool has_result;
  u16 log;
  u64 lsn;
};

struct Worker {
  u16 id = 0;
  Server* srv = nullptr;
  std::unique_ptr<net::ConnSet> cs;
  std::vector<Pending> pending;
  u64 seq = 0;
  u64 requests = 0;
};

wire::RespHeader header(const wire::ReqHeader& q, Status s, const ObjectID& id) {
  wire::RespHeader h{};
  h.len = static_cast<u32>(sizeof(h));
  h.kind = wire::Kind::RESP;
  h.status = static_cast<u16>(s);
  h.req_id = q.req_id;
  h.id = id;
  h.map_epoch = 1;
  return h;
}

void respond(Worker& w, net::ConnSet::Conn* c, wire::RespHeader h, const BalanceResult* r) {
  if (!w.srv->logs.empty()) h.flags |= wire::kRespDurable;
  if (r) h.len += sizeof(*r);
  w.cs->respond(c, h, r ? ByteView(r, sizeof(*r)) : ByteView{});
}

// Responds now if nothing needs to become durable, else parks the response.
void finish(Worker& w, net::ConnSet::Conn* c, const wire::RespHeader& h, const BalanceResult* r,
            u16 log, u64 lsn) {
  Server& s = *w.srv;
  if (s.logs.empty() || lsn == 0 || lsn <= s.logs[log]->durable()) {
    respond(w, c, h, r);
    return;
  }
  net::ConnSet::hold(c);
  w.pending.push_back(Pending{c, h, r ? *r : BalanceResult{}, r != nullptr, log, lsn});
}

bool complete(Worker& w) {
  std::size_t k = 0;
  for (Pending& p : w.pending) {
    if (p.lsn <= w.srv->logs[p.log]->durable()) {
      respond(w, p.conn, p.h, p.has_result ? &p.result : nullptr);
      net::ConnSet::release(p.conn);
    } else {
      w.pending[k++] = p;
    }
  }
  bool progressed = k != w.pending.size();
  w.pending.resize(k);
  return progressed;
}

void on_request(void* user, net::ConnSet::Conn* c, const wire::ReqHeader& q, ByteView args) {
  Worker& w = *static_cast<Worker*>(user);
  Server& s = *w.srv;
  ++w.requests;
  switch (q.kind) {
    case wire::Kind::TOPOLOGY: {
      SlotMap m = SlotMap::make_default(s.threads);
      wire::TopologyBody b{};
      b.map_epoch = m.epoch;
      b.n_shards = m.n_shards;
      std::memcpy(b.owner, m.owner.data(), sizeof(b.owner));
      wire::RespHeader h = header(q, Status::OK, kNullId);
      h.len += sizeof(b);
      w.cs->respond(c, h, ByteView(&b, sizeof(b)));
      return;
    }
    case wire::Kind::EXEC: {
      if (q.op == kOpTransfer) {
        if (args.len != sizeof(TransferArgs)) {
          respond(w, c, header(q, Status::E_BAD_ARGS, q.target), nullptr);
          return;
        }
        TransferArgs x;
        std::memcpy(&x, args.data, sizeof(x));
        Wait wt = transfer_op(s.map, s.logs, w.id, q.target, x.to, x.amount, now_ns());
        if (!wt.found || wt.version == 0) {
          wire::RespHeader h = header(q, wt.found ? Status::E_APP : Status::E_NOT_FOUND, q.target);
          if (wt.found) h.app_code = kAppInsufficientFunds;
          respond(w, c, h, nullptr);
          return;
        }
        wire::RespHeader h = header(q, Status::OK, q.target);
        h.version = wt.version;
        BalanceResult r{wt.balance, wt.op_count};
        finish(w, c, h, &r, wt.log, wt.lsn);
        return;
      }
      bool read = q.op == kOpGetBalance;
      if (!read && (q.op != kOpDeposit || args.len != sizeof(AmountArgs))) {
        respond(w, c, header(q, Status::E_BAD_ARGS, q.target), nullptr);
        return;
      }
      Wait wt = execute_op(s.map, s.logs, w.id, q.target, read, now_ns());
      if (!wt.found) {
        respond(w, c, header(q, Status::E_NOT_FOUND, q.target), nullptr);
        return;
      }
      wire::RespHeader h = header(q, Status::OK, q.target);
      h.version = wt.version;
      BalanceResult r{wt.balance, wt.op_count};
      finish(w, c, h, read ? &r : nullptr, wt.log, wt.lsn);
      return;
    }
    case wire::Kind::CREATE: {
      if (q.op != kTypeId || args.len != sizeof(CreateArgs)) {
        respond(w, c, header(q, Status::E_BAD_ARGS, kNullId), nullptr);
        return;
      }
      CreateArgs a;
      std::memcpy(&a, args.data, sizeof(a));
      auto* o = new Obj;
      o->id = ObjectID::make(1 + w.id, ++w.seq);
      o->version = 1;
      o->a.balance_cents = a.balance_cents;
      o->a.created_ns = a.created_ns;
      o->a.email_len = std::min<u8>(a.email_len, sizeof(o->a.email));
      o->a.name_len = std::min<u8>(a.name_len, sizeof(o->a.name));
      std::memcpy(o->a.email, a.email, o->a.email_len);
      std::memcpy(o->a.name, a.name, o->a.name_len);
      insert_object(s.map, s.logs, w.id, o);
      wire::RespHeader h = header(q, Status::OK, o->id);
      h.version = 1;
      finish(w, c, h, nullptr, o->last_log, o->last_lsn);
      return;
    }
    default:
      respond(w, c, header(q, Status::E_UNKNOWN_OP, q.target), nullptr);
  }
}

void worker_main(Worker& w, int cpu) {
  if (cpu >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  }
  u64 idle_since = 0;
  while (!w.srv->stop.load(std::memory_order_relaxed)) {
    bool did = w.cs->poll();
    if (!w.pending.empty() && complete(w)) {
      w.cs->flush();
      did = true;
    }
    if (did || !w.pending.empty()) {
      idle_since = 0;
      continue;
    }
    // Same idle policy as an SNC shard: spin 50 us, then sleep on the sockets.
    u64 now = now_ns();
    if (idle_since == 0) idle_since = now;
    if (now - idle_since < 50000) continue;
    pollfd p{w.cs->wait_fd(), POLLIN, 0};
    ::poll(&p, 1, 1);
    idle_since = 0;
  }
}

std::vector<int> parse_cpus(const char* s) {
  std::vector<int> v;
  for (const char* p = s; *p;) {
    v.push_back(std::atoi(p));
    while (*p && *p != ',') ++p;
    if (*p == ',') ++p;
  }
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  Server srv;
  std::vector<int> cpus;
  std::string host = "127.0.0.1", data_dir;
  u16 port = 7400, n_logs = 1;
  u64 objects = 1000000;
  u32 group_commit_us = 50;
  bool prefetch = true;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto val = [&] {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", k.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (k == "--threads") srv.threads = static_cast<u16>(std::atoi(val()));
    else if (k == "--cpus") cpus = parse_cpus(val());
    else if (k == "--host") host = val();
    else if (k == "--port") port = static_cast<u16>(std::atoi(val()));
    else if (k == "--objects") objects = std::strtoull(val(), nullptr, 10);
    else if (k == "--data-dir") data_dir = val();
    else if (k == "--logs") n_logs = static_cast<u16>(std::max(1, std::atoi(val())));
    else if (k == "--group-commit-us") group_commit_us = static_cast<u32>(std::atoi(val()));
    else if (k == "--no-prefetch") prefetch = false;
    else {
      std::fprintf(stderr, "unknown flag %s\n", k.c_str());
      return 2;
    }
  }
  if (srv.threads == 0) {
    srv.threads = cpus.empty() ? static_cast<u16>(std::max(1u, std::thread::hardware_concurrency()))
                               : static_cast<u16>(cpus.size());
  }
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);

  for (std::size_t i = 0; i < kStripes; ++i) srv.map.stripes[i].map.reserve(objects / kStripes * 2);
  if (!data_dir.empty()) {
    // The log writers run on the server's cores, as in baseline_bench.
    for (u16 i = 0; i < n_logs; ++i) {
      srv.logs.push_back(std::make_unique<SharedLog>(data_dir, i, group_commit_us, true));
    }
  }
  std::vector<std::unique_ptr<Worker>> workers;
  for (u16 i = 0; i < srv.threads; ++i) {
    auto w = std::make_unique<Worker>();
    w->id = i;
    w->srv = &srv;
    w->cs = std::make_unique<net::ConnSet>(host, static_cast<u16>(port + i), on_request, w.get());
    if (prefetch) {
      w->cs->set_on_batch([](void* user, const ObjectID* ids, u32 n) {
        static_cast<Worker*>(user)->srv->map.prefetch(ids, n);
      });
    }
    workers.push_back(std::move(w));
  }
  std::vector<std::thread> ts;
  for (u16 i = 0; i < srv.threads; ++i) {
    int cpu = cpus.empty() ? -1 : cpus[i % cpus.size()];
    ts.emplace_back([&, i, cpu] { worker_main(*workers[i], cpu); });
  }
  std::printf("baseline_server threads=%u ports %u-%u durability=%s logs=%zu\n", srv.threads, port,
              port + srv.threads - 1, data_dir.empty() ? "none (in memory)" : "DURABLE (WAL, RWF_DSYNC)",
              srv.logs.size());
  std::fflush(stdout);
  int sig = 0;
  sigwait(&set, &sig);
  srv.stop = true;
  for (auto& t : ts) t.join();
  u64 req = 0, contended = srv.map.contended.load();
  for (auto& w : workers) req += w->requests;
  std::printf("total: requests %llu contended stripe locks %.3f%%\n", (unsigned long long)req,
              100.0 * contended / std::max<u64>(1, req));
  // Parked responses hold connection refs; drop them with the workers.
  return 0;
}
