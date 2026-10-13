// Socket front end and owner-aware client (docs/ROUTING.md).
//
// ConnSet: one listening TCP socket and its connections, polled from one
//   thread with a non-blocking epoll. Reads request frames, hands them to a
//   callback, buffers responses and writes them back. Knows nothing about
//   SNC execution, so the thread-pool baseline uses it too.
// NetServer: one ConnSet per shard on port base_port + shard, polled from the
//   shard's own loop (Engine::Config::app_poll). Requests run through the
//   normal API, so a request that reaches the wrong shard is forwarded.
// Client: one connection per shard; fetches the slot map once and sends each
//   request to the connection of the shard that owns its target.
#pragma once

#include <array>
#include <memory>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "snc/wire.h"

namespace snc::net {

class ConnSet {
 public:
  struct Conn;
  using OnRequest = void (*)(void* user, Conn* c, const wire::ReqHeader& h, ByteView args);

  // Binds and listens on host:port; aborts if it cannot.
  ConnSet(const std::string& host, u16 port, OnRequest on_request, void* user);
  ~ConnSet();
  ConnSet(const ConnSet&) = delete;
  ConnSet& operator=(const ConnSet&) = delete;

  // Optional: called with the targets of each batch of whole frames read
  // from one connection, before any of them is dispatched.
  using OnBatch = void (*)(void* user, const ObjectID* targets, u32 n);
  void set_on_batch(OnBatch fn) { on_batch_ = fn; }

  // Readable when sockets have work; for sri_watch_fd or an idle poll().
  int wait_fd() const { return epfd_; }

  // Accepts, reads and dispatches whole frames, then writes queued
  // responses. Returns true if it did any work.
  bool poll();
  // Writes queued responses without reading.
  void flush();

  // Queues a response on `c`. Dropped if the connection has closed.
  void respond(Conn* c, const wire::RespHeader& h, ByteView body);
  // Keeps `c` valid across an asynchronous completion.
  static void hold(Conn* c);
  static void release(Conn* c);

  struct Stats {
    u64 accepted = 0;
    u64 frames_in = 0, frames_out = 0;
    u64 bytes_in = 0, bytes_out = 0;
    u64 recv_calls = 0, send_calls = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  void accept_all();
  bool read_conn(Conn* c);
  void close_conn(Conn* c);

  int listen_fd_ = -1;
  int epfd_ = -1;
  OnRequest on_request_;
  OnBatch on_batch_ = nullptr;
  void* user_;
  std::vector<ObjectID> batch_;
  std::vector<Conn*> conns_;  // open connections
  std::vector<Conn*> dirty_;  // connections with queued output (each holds a ref)
  Stats stats_;
};

// SNC server front end. Construct before Engine::start() (it binds the ports),
// and set Engine::Config::app_poll = NetServer::poll_hook, app_user = this.
class NetServer {
 public:
  struct Config {
    std::string host = "127.0.0.1";
    u16 base_port = 7400;
  };
  struct Stats {
    ConnSet::Stats conn;
    u64 requests = 0;
    u64 forwarded = 0;  // target owned by another shard
  };

  NetServer(const Config& cfg, u16 n_shards);
  ~NetServer();

  static bool poll_hook(void* self, u16 shard);
  // Racy while running.
  Stats stats(u16 shard) const;

  struct PerShard;

 private:
  Config cfg_;
  std::vector<std::unique_ptr<PerShard>> shards_;
};

class Client {
 public:
  using OnResponse = void (*)(void* ctx, const wire::RespHeader& h, ByteView body);

  Client() = default;
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Connects to shard 0, fetches the slot map, then connects to every shard.
  // Returns false (with errno-style text in error()) if any step fails.
  bool connect(const std::string& host, u16 base_port);
  const std::string& error() const { return error_; }

  // Failover (docs/RETRIES.md). Other nodes serving the same space, tried
  // in turn when a connection fails, until one answers as primary (within
  // failover_ms). With a session, every request not yet answered is then
  // resent in its original order: EXEC and CREATE requests carry a
  // per-shard sequence, so the new primary answers one already applied from
  // its session (kRespReplayed) instead of running it twice. Without a
  // session, a failure still ends poll() with -1.
  //
  // request_timeout_ms (sessions only; 0 = off): a node that leaves a request
  // unanswered that long counts as failed, as a broken connection does. It
  // catches a primary that is alive but cut off (fenced, it acknowledges
  // nothing) or hung. It also bounds each step of reconnecting. A false
  // timeout costs a reconnect and a resend, never a double apply.
  void add_endpoint(const std::string& host, u16 base_port);
  void enable_session(u64 client_id, int failover_ms = 30000, int request_timeout_ms = 5000);
  u64 timeouts() const { return timeouts_; }
  u64 failovers() const { return failovers_; }

  u16 shard_count() const { return map_.n_shards; }
  const SlotMap& slot_map() const { return map_; }
  u16 owner_of(const ObjectID& id) const { return map_.owner_of(id); }

  // Queues a request on shard `shard`'s connection. Sent by poll().
  void send(u16 shard, const wire::ReqHeader& h, ByteView args);
  // Sends queued requests, then reads responses and calls `on` for each.
  // Waits up to timeout_ms for the first response when none is ready.
  // Returns the number of responses, or -1 if a connection failed.
  int poll(OnResponse on, void* ctx, int timeout_ms);

  u64 forwarded() const { return forwarded_; }
  // Readable when a response may be waiting (for polling several clients).
  int wait_fd() const { return epfd_; }

 private:
  struct Link {
    int fd = -1;
    std::vector<u8> out;
    std::size_t out_off = 0;
    std::vector<u8> in;
    std::size_t in_len = 0;
    // Session: unanswered requests in send order, to resend after a failover.
    u64 next_seq = 1;
    u64 base = 0;  // order of unanswered.front()
    std::deque<std::vector<u8>> unanswered;  // emptied once answered
    std::unordered_map<u64, u64> order;      // req_id -> its order
  };
  bool flush_all();
  int read_link(Link& l, OnResponse on, void* ctx);
  bool open(const std::string& host, u16 base_port);
  void close_links();
  bool fail_over();
  struct Endpoint {
    std::string host;
    u16 port;
  };
  std::vector<Endpoint> endpoints_;
  std::size_t current_ = 0;
  u64 client_id_ = 0;
  int failover_ms_ = 30000;
  int request_timeout_ms_ = 0;
  u64 failovers_ = 0;
  u64 timeouts_ = 0;
  // Last answer, or when the client last had nothing unanswered (steady ns).
  i64 progress_ns_ = 0;

  SlotMap map_;
  std::vector<Link> links_;
  int epfd_ = -1;
  u64 forwarded_ = 0;
  std::string error_;
};

// Clustered spaces (docs/CLUSTER.md): fetches the cluster map from any node,
// keeps a Client per replication group, and sends each request to the
// group, and the shard in it, that owns its target. A single space works
// too (one group owning every slot). Each group's Client connects to
// whichever member answers as primary; with sessions it also follows the
// group's failovers (docs/RETRIES.md), which never change the map.
class ClusterClient {
 public:
  ClusterClient() = default;
  ~ClusterClient();
  ClusterClient(const ClusterClient&) = delete;
  ClusterClient& operator=(const ClusterClient&) = delete;

  // Before connect: every group's Client gets a session (client_id), so
  // it fails over to another member of its group and resends safely.
  void enable_sessions(u64 client_id, int failover_ms = 30000, int request_timeout_ms = 5000);
  // Asks host:base_port for the cluster map, then connects to every group.
  bool connect(const std::string& host, u16 base_port);
  const std::string& error() const { return error_; }

  u16 group_count() const { return static_cast<u16>(groups_.size()); }
  u16 group_of(const ObjectID& id) const { return group_of_[slot_of(id)]; }
  u32 map_crc() const { return crc_; }
  u64 epoch() const { return epoch_; }
  Client& group(u16 g) { return *groups_[g]; }

  // Queues a request on the owner of h.target (a create with a null target
  // goes to `create_group`).
  void send(const wire::ReqHeader& h, ByteView args, u16 create_group = 0);
  // Polls every group's client; waits up to timeout_ms for the first
  // response. Returns responses handled, or -1 if a group's client failed.
  int poll(Client::OnResponse on, void* ctx, int timeout_ms);

 private:
  std::vector<std::unique_ptr<Client>> groups_;
  std::array<u8, kSlots> group_of_{};
  u32 crc_ = 0;
  u64 epoch_ = 0;
  u64 client_id_ = 0;
  int failover_ms_ = 30000, request_timeout_ms_ = 5000;
  int epfd_ = -1;
  std::string error_;
};

}  // namespace snc::net
