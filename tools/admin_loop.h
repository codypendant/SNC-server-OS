// snc_server's admin port (docs/HEALTH_DECOUPLING.md 3.3): a non-blocking
// poll() loop that never waits on shard threads.
//
//   HEALTH          answered inline, from published state only
//   MOVE ...        handed to its own thread as soon as "MOVE " is seen;
//                   that thread reads the rest of the (possibly large) line
//   anything else   queued to ONE worker thread and answered in order, so
//                   STATUS, AUDIT, PROMOTE ... keep their one-at-a-time
//                   behavior; a full queue answers "NO busy"
//
// Bounds: kQueueMax queued heavy requests; kHalfMax connections whose
// request line is incomplete (the oldest is closed to admit a new one); a
// line must arrive within kReadMs of the accept (as ha::read_line's 500 ms
// before this loop); a non-MOVE line longer than kLineMax is refused.
//
// The counters are atomics, read by HEALTH (inexpensive observability, as
// the review of PR #36 asked; no metrics subsystem).
#pragma once

#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ha.h"
#include "snc/trace.h"
#include "snc/types.h"

namespace snc::admin {

inline constexpr std::size_t kQueueMax = 16;   // ACCEPTED (review of PR #36, decision 2)
inline constexpr std::size_t kHalfMax = 256;   // ACCEPTED (decision 3)
inline constexpr int kReadMs = 500;
inline constexpr std::size_t kLineMax = 64 * 1024;

struct Counters {
  std::atomic<unsigned long long> queue_depth{0}, queue_hw{0}, busy{0};
  std::atomic<unsigned long long> half{0}, half_hw{0}, evicted{0}, timeouts{0};
};

inline u64 mono_ns() {
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
}

// For a MOVE handler: completes the line whose first bytes are `got`
// (blocking fd), by `deadline_ns` (mono_ns). Keeps the line without its
// newline, as ha::read_line does.
inline bool finish_line(int fd, std::string& got, u64 deadline_ns) {
  auto nl = got.find('\n');
  char buf[65536];
  while (nl == std::string::npos) {
    if (got.size() >= (std::size_t{1} << 28)) return false;
    const u64 now = mono_ns();
    if (now >= deadline_ns || !ha::wait_fd(fd, POLLIN, static_cast<int>((deadline_ns - now) / 1000000) + 1)) {
      return false;
    }
    ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return false;
    const std::size_t old = got.size();
    got.append(buf, static_cast<std::size_t>(r));
    const auto p = got.find('\n', old);
    if (p != std::string::npos) nl = p;
  }
  got.resize(nl);
  return true;
}

class Loop {
 public:
  struct Handlers {
    // Inline verbs: answered on the loop thread. Each MUST NOT block (no
    // shard task, no lock a shard thread holds). Return false if `line` is
    // not one of them.
    std::function<bool(const std::string& line, std::string& reply)> inline_verb;
    // Heavy verbs, on the worker thread, one at a time.
    std::function<std::string(const std::string& line)> heavy;
    // MOVE: called on a new thread with the connection (blocking mode) and
    // the bytes read so far; it reads the rest of the line itself, answers
    // and closes the fd. Null: MOVE goes to `heavy` like any other verb.
    std::function<void(int fd, std::string got, u64 deadline_ns)> move;
  };

  Loop(int listen_fd, Handlers h) : lfd_(listen_fd), h_(std::move(h)) {
    ::fcntl(lfd_, F_SETFL, ::fcntl(lfd_, F_GETFL) | O_NONBLOCK);
    loop_ = std::thread([this] { run(); });
    worker_ = std::thread([this] { work(); });
  }
  ~Loop() { stop(); }

  // Stop accepting, close the half-read connections, answer what is
  // queued "NO stopping", and join both threads. A heavy request already
  // running finishes first. MOVE threads are the caller's to wait for.
  void stop() {
    if (stopped_.exchange(true)) return;
    stop_.store(true);
    if (loop_.joinable()) loop_.join();
    {
      std::lock_guard<std::mutex> g(mu_);
      worker_stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

  const Counters& counters() const { return c_; }

 private:
  struct Conn {
    int fd;
    u64 accepted_ns;
    std::string buf;
  };
  struct Job {
    int fd;
    std::string line;
    u64 accepted_ns;
  };

  static void reply_close(int fd, const std::string& reply) {
    // Small replies on a fresh connection: the send buffer takes them.
    const std::string s = reply + "\n";
    ssize_t r;
    do r = ::send(fd, s.data(), s.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    while (r < 0 && errno == EINTR);
    ::close(fd);
  }

  void set_half(std::size_t n) {
    c_.half.store(n);
    if (n > c_.half_hw.load()) c_.half_hw.store(n);
  }

  void run() {
    std::deque<Conn> half;  // oldest first
    std::vector<pollfd> pfds;
    while (!stop_.load()) {
      pfds.clear();
      pfds.push_back({lfd_, POLLIN, 0});
      for (const Conn& c : half) pfds.push_back({c.fd, POLLIN, 0});
      // Wake for the next read deadline, or to see stop_.
      int timeout = 100;
      const u64 now0 = mono_ns();
      for (const Conn& c : half) {
        const u64 dl = c.accepted_ns + u64{kReadMs} * 1000000;
        const int ms = dl <= now0 ? 0 : static_cast<int>((dl - now0) / 1000000) + 1;
        if (ms < timeout) timeout = ms;
      }
      int n = ::poll(pfds.data(), pfds.size(), timeout);
      if (n < 0 && errno != EINTR) break;
      const u64 now = mono_ns();
      // Read what arrived. pfds[i + 1] is half[i] (half is not changed
      // before this pass ends; finished ones are marked fd = -1).
      for (std::size_t i = 0; n > 0 && i < half.size(); ++i) {
        if (pfds[i + 1].revents == 0) continue;
        on_readable(half[i], now);
      }
      // Drop the finished, the timed out.
      for (auto it = half.begin(); it != half.end();) {
        if (it->fd >= 0 && now - it->accepted_ns >= u64{kReadMs} * 1000000) {
          SNC_TRACE("admin_read_fail read_ms=%.1f bytes=%zu", (now - it->accepted_ns) / 1e6, it->buf.size());
          ::close(it->fd);
          it->fd = -1;
          c_.timeouts.fetch_add(1);
        }
        it = it->fd < 0 ? half.erase(it) : it + 1;
      }
      // Accept everything waiting.
      if (n > 0 && (pfds[0].revents & POLLIN)) {
        while (true) {
          int fd = ::accept4(lfd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
          if (fd < 0) {
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
              // Out of descriptors: the listener stays readable, so free
              // one (the oldest half-read connection) or back off rather
              // than spin.
              if (!half.empty()) {
                ::close(half.front().fd);
                half.pop_front();
                c_.evicted.fetch_add(1);
              } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
              }
            }
            break;
          }
          if (half.size() >= kHalfMax) {
            // Evict the oldest: a client that sends its line at once is
            // never the oldest for long (HEALTH completes in microseconds).
            ::close(half.front().fd);
            half.pop_front();
            c_.evicted.fetch_add(1);
          }
          half.push_back(Conn{fd, now, {}});
          // Most requests are already in the socket: try at once.
          on_readable(half.back(), now);
          if (half.back().fd < 0) half.pop_back();
        }
      }
      set_half(half.size());
    }
    for (Conn& c : half) ::close(c.fd);
    set_half(0);
    ::close(lfd_);
  }

  // Reads what the connection has; on a whole line (or MOVE's prefix)
  // dispatches it and marks the connection finished (fd = -1).
  void on_readable(Conn& c, u64 now) {
    char buf[16384];
    while (true) {
      ssize_t r = ::recv(c.fd, buf, sizeof(buf), 0);
      if (r < 0 && errno == EINTR) continue;
      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      if (r <= 0) {  // closed or failed before a whole line
        SNC_TRACE("admin_read_fail read_ms=%.1f bytes=%zu", (now - c.accepted_ns) / 1e6, c.buf.size());
        ::close(c.fd);
        c.fd = -1;
        return;
      }
      c.buf.append(buf, static_cast<std::size_t>(r));
      if (c.buf.size() >= 5 && h_.move && c.buf.compare(0, 5, "MOVE ") == 0) {
        // A slot's objects may follow: the move thread reads the rest, so
        // a large line never passes through this loop.
        int fd = c.fd;
        c.fd = -1;
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        h_.move(fd, std::move(c.buf), c.accepted_ns + u64{kReadMs} * 1000000);
        return;
      }
      const auto nl = c.buf.find('\n');
      if (nl != std::string::npos) {
        dispatch(c.fd, c.buf.substr(0, nl), c.accepted_ns);
        c.fd = -1;
        return;
      }
      if (c.buf.size() > kLineMax) {
        reply_close(c.fd, "NO line too long");
        c.fd = -1;
        return;
      }
    }
  }

  void dispatch(int fd, const std::string& line, u64 accepted_ns) {
    std::string reply;
    if (h_.inline_verb && h_.inline_verb(line, reply)) {
      reply_close(fd, reply);
      if (trace::enabled()) {
        const u64 ms = (mono_ns() - accepted_ns) / 1000000;
        if (ms >= 20) SNC_TRACE("admin_inline_slow verb=%.16s total_ms=%llu", line.c_str(), (unsigned long long)ms);
      }
      return;
    }
    std::unique_lock<std::mutex> g(mu_);
    if (q_.size() >= kQueueMax) {
      g.unlock();
      c_.busy.fetch_add(1);
      reply_close(fd, "NO busy");
      return;
    }
    q_.push_back(Job{fd, line, accepted_ns});
    c_.queue_depth.store(q_.size());
    if (q_.size() > c_.queue_hw.load()) c_.queue_hw.store(q_.size());
    g.unlock();
    cv_.notify_one();
  }

  void work() {
    while (true) {
      Job j;
      {
        std::unique_lock<std::mutex> g(mu_);
        cv_.wait(g, [&] { return worker_stop_ || !q_.empty(); });
        if (q_.empty()) return;  // stopping, nothing left
        j = std::move(q_.front());
        q_.pop_front();
        c_.queue_depth.store(q_.size());
        if (worker_stop_) {
          g.unlock();
          reply_close(j.fd, "NO stopping");
          continue;
        }
      }
      const u64 t_run = mono_ns();
      const std::string reply = h_.heavy(j.line);
      ::fcntl(j.fd, F_SETFL, ::fcntl(j.fd, F_GETFL) & ~O_NONBLOCK);
      ha::write_all(j.fd, reply + "\n");
      ::close(j.fd);
      if (trace::enabled()) {
        const u64 t_end = mono_ns();
        if (t_end - j.accepted_ns >= 20'000'000) {
          SNC_TRACE("admin_heavy_slow verb=%.16s total_ms=%.1f queue_ms=%.1f run_ms=%.1f", j.line.c_str(),
                    (t_end - j.accepted_ns) / 1e6, (t_run - j.accepted_ns) / 1e6, (t_end - t_run) / 1e6);
        }
      }
    }
  }

  int lfd_;
  Handlers h_;
  Counters c_;
  std::atomic<bool> stop_{false}, stopped_{false};
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> q_;
  bool worker_stop_ = false;
  std::thread loop_, worker_;
};

}  // namespace snc::admin
