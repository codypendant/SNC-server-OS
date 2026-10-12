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

#include <memory>
#include <string>
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
  void* user_;
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

 private:
  struct Link {
    int fd = -1;
    std::vector<u8> out;
    std::size_t out_off = 0;
    std::vector<u8> in;
    std::size_t in_len = 0;
  };
  bool flush_all();
  int read_link(Link& l, OnResponse on, void* ctx);

  SlotMap map_;
  std::vector<Link> links_;
  int epfd_ = -1;
  u64 forwarded_ = 0;
  std::string error_;
};

}  // namespace snc::net
