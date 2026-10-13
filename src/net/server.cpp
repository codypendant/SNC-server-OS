// NetServer: one ConnSet per shard, polled from the shard's own loop. Every
// request goes through the ordinary API on the shard that received it, so a
// request for another shard's object takes the cross-shard path (and the
// response says so); a request sent to the owner never leaves its core.
#include <memory>

#include "snc/net.h"
#include "snc/snc.h"

namespace snc::net {

struct NetServer::PerShard {
  explicit PerShard(const std::string& host, u16 port)
      : cs(host, port, &PerShard::on_request, this) {
    cs.set_on_batch([](void*, const ObjectID* ids, u32 n) { prefetch(ids, n); });
  }

  struct Req {
    PerShard* ps;
    ConnSet::Conn* conn;
    u64 req_id;
    bool forwarded;
  };

  static void on_request(void* user, ConnSet::Conn* c, const wire::ReqHeader& h, ByteView args);
  static void on_done(void* ctx, const ExecResult& r);
  Req* get_req() {
    if (free_reqs.empty()) {
      all_reqs.push_back(std::make_unique<Req>());
      return all_reqs.back().get();
    }
    Req* r = free_reqs.back();
    free_reqs.pop_back();
    return r;
  }

  ConnSet cs;
  bool watched = false;
  u64 requests = 0, forwarded = 0;
  std::vector<std::unique_ptr<Req>> all_reqs;
  std::vector<Req*> free_reqs;
};

namespace {
wire::RespHeader resp_header(u64 req_id, Status s, const ObjectID& id, u32 body_len) {
  wire::RespHeader h{};
  h.len = static_cast<u32>(sizeof(h)) + body_len;
  h.kind = wire::Kind::RESP;
  h.status = static_cast<u16>(s);
  h.req_id = req_id;
  h.id = id;
  h.owner = owner_of(id);
  h.map_epoch = slot_map().epoch;
  return h;
}
}  // namespace

void NetServer::PerShard::on_request(void* user, ConnSet::Conn* c, const wire::ReqHeader& h,
                                     ByteView args) {
  auto* ps = static_cast<PerShard*>(user);
  ++ps->requests;
  if (h.kind == wire::Kind::TOPOLOGY) {
    const SlotMap& m = slot_map();
    wire::TopologyBody b{};
    b.map_epoch = m.epoch;
    b.n_shards = m.n_shards;
    std::memcpy(b.owner, m.owner.data(), sizeof(b.owner));
    ps->cs.respond(c, resp_header(h.req_id, Status::OK, kNullId, sizeof(b)), ByteView(&b, sizeof(b)));
    return;
  }
  if (h.kind == wire::Kind::CLUSTER) {
    const ClusterMap* m = cluster_map();
    const u16 n = m ? static_cast<u16>(m->nodes.size()) : 0;
    std::vector<u8> body(sizeof(wire::ClusterBody) + n * sizeof(wire::ClusterEntry));
    auto* b = reinterpret_cast<wire::ClusterBody*>(body.data());
    b->n_nodes = n;
    if (m) {
      b->crc = m->crc();
      b->node_id = node_id();
      std::memcpy(b->node_of, m->node_of.data(), kSlots);
      auto* e = reinterpret_cast<wire::ClusterEntry*>(b + 1);
      for (u16 i = 0; i < n; ++i) {
        std::strncpy(e[i].host, m->nodes[i].host.c_str(), sizeof(e[i].host) - 1);
        e[i].client_port = m->nodes[i].client_port;
        e[i].n_shards = m->nodes[i].n_shards;
      }
    }
    ps->cs.respond(c, resp_header(h.req_id, Status::OK, kNullId, static_cast<u32>(body.size())),
                   ByteView(body.data(), static_cast<u32>(body.size())));
    return;
  }
  ExecOptions opts;
  opts.expected_version = h.expected_version;
  opts.allow_undurable = h.flags & wire::kReqAllowUndurable;
  if (h.flags & wire::kReqApplied) opts.durability = Durability::APPLIED;
  if (h.flags & wire::kReqSequenced) {
    wire::SeqPrefix sp{};
    if (args.len >= sizeof(sp)) std::memcpy(&sp, args.data, sizeof(sp));
    opts.client_id = sp.client_id;
    opts.seq = sp.seq;
    args = args.len >= sizeof(sp) ? ByteView(args.data + sizeof(sp), args.len - sizeof(sp)) : ByteView{};
  }

  Req* r = ps->get_req();
  r->ps = ps;
  r->conn = c;
  r->req_id = h.req_id;
  r->forwarded = (h.kind != wire::Kind::CREATE || !h.target.is_null()) &&
                 owner_of(h.target) != sri_shard_id();
  ps->forwarded += r->forwarded;
  ConnSet::hold(c);
  switch (h.kind) {
    case wire::Kind::EXEC: execute(h.target, h.op, args, opts, on_done, r); break;
    case wire::Kind::CREATE:
      if (h.target.is_null()) {
        create(h.op, args, opts, on_done, r);
      } else {
        create_near(h.target, h.op, args, opts, on_done, r);
      }
      break;
    case wire::Kind::DELETE: remove(h.target, opts, on_done, r); break;
    default: {
      ExecResult bad;
      bad.status = Status::E_BAD_ARGS;
      bad.id = h.target;
      on_done(r, bad);
    }
  }
}

void NetServer::PerShard::on_done(void* ctx, const ExecResult& r) {
  auto* q = static_cast<Req*>(ctx);
  PerShard* ps = q->ps;
  wire::RespHeader h = resp_header(q->req_id, r.status, r.id, r.result.len);
  h.app_code = r.app_code;
  h.version = r.version;
  if (r.durable) h.flags |= wire::kRespDurable;
  if (q->forwarded) h.flags |= wire::kRespForwarded;
  if (r.replayed) h.flags |= wire::kRespReplayed;
  ps->cs.respond(q->conn, h, r.result);
  ConnSet::release(q->conn);
  ps->free_reqs.push_back(q);
}

NetServer::NetServer(const Config& cfg, u16 n_shards) : cfg_(cfg) {
  for (u16 s = 0; s < n_shards; ++s) {
    shards_.push_back(std::make_unique<PerShard>(cfg.host, static_cast<u16>(cfg.base_port + s)));
  }
}

NetServer::~NetServer() = default;

bool NetServer::poll_hook(void* self, u16 shard) {
  PerShard& ps = *static_cast<NetServer*>(self)->shards_[shard];
  if (!ps.watched) {
    sri_watch_fd(ps.cs.wait_fd());
    ps.watched = true;
  }
  return ps.cs.poll();
}

NetServer::Stats NetServer::stats(u16 shard) const {
  const PerShard& ps = *shards_.at(shard);
  Stats s;
  s.conn = ps.cs.stats();
  s.requests = ps.requests;
  s.forwarded = ps.forwarded;
  return s;
}

}  // namespace snc::net
