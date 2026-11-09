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
// Replies from the loop (S4.1, docs/S4_SYNC_TOKENS.md section 9): sent
// without blocking; what the socket does not take at once is kept and
// sent as the client reads, on POLLOUT, so a large reply (GAUGES) is
// delivered whole and a slow reader never holds HEALTH. A reply not fully
// sent within the write deadline (kWriteMs by default), or whose send
// fails, is closed unfinished and counted: every reply ends in a newline,
// so a client sees a cut reply as a missing newline, never as a complete
// one. At most kOutMax replies wait; a new one past that closes the
// oldest waiting (counted).
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
inline constexpr int kWriteMs = 5000;
inline constexpr std::size_t kOutMax = 64;

struct Counters {
  std::atomic<unsigned long long> queue_depth{0}, queue_hw{0}, busy{0};
  std::atomic<unsigned long long> half{0}, half_hw{0}, evicted{0}, timeouts{0};
  // S4.1: replies waiting for the client to read; closed unfinished at the
  // write deadline, on a send error, or to admit a newer one past kOutMax.
  std::atomic<unsigned long long> out{0}, out_hw{0}, write_timeouts{0}, write_failed{0}, write_evicted{0};
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

  // write_ms: the reply write deadline; out_max: replies that may wait
  // (tests lower both).
  Loop(int listen_fd, Handlers h, int write_ms = kWriteMs, std::size_t out_max = kOutMax)
      : lfd_(listen_fd), h_(std::move(h)), write_ms_(write_ms), out_max_(out_max) {
    ::fcntl(lfd_, F_SETFL, ::fcntl(lfd_, F_GETFL) | O_NONBLOCK);
  }
  // Starts answering. Separate from the constructor so the caller can
  // publish the Loop (its counters) before the first request.
  void start() {
    loop_ = std::thread([this] { run(); });
    worker_ = std::thread([this] { work(); });
  }
  ~Loop() { stop(); }

  // Stop accepting, close the half-read connections, answer what is
  // queued "NO stopping", and join both threads. A heavy request already
  // running finishes first. MOVE threads are the caller's to wait for.
  void stop() {
    if (stopped_.exchange(true)) return;
    if (!loop_.joinable()) {  // never started
      ::close(lfd_);
      return;
    }
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

  struct Out {
    int fd;
    std::string buf;
    std::size_t off;
    u64 deadline_ns;
  };
  enum class Sent { DONE, BLOCKED, FAILED };

  // As much as the socket takes now.
  static Sent send_some(Out& o) {
    while (o.off < o.buf.size()) {
      const ssize_t r = ::send(o.fd, o.buf.data() + o.off, o.buf.size() - o.off, MSG_NOSIGNAL | MSG_DONTWAIT);
      if (r < 0 && errno == EINTR) continue;
      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return Sent::BLOCKED;
      if (r <= 0) return Sent::FAILED;
      o.off += static_cast<std::size_t>(r);
    }
    return Sent::DONE;
  }

  // A reply from the loop thread: sent now if the socket takes it all,
  // otherwise kept for POLLOUT until the write deadline. Never blocks.
  void reply(int fd, const std::string& text, u64 now) {
    Out o{fd, text + "\n", 0, now + static_cast<u64>(write_ms_) * 1000000};
    switch (send_some(o)) {
      case Sent::DONE: ::close(fd); return;
      case Sent::FAILED:
        ::close(fd);
        c_.write_failed.fetch_add(1);
        return;
      case Sent::BLOCKED: break;
    }
    if (out_.size() >= out_max_) {
      ::close(out_.front().fd);
      out_.pop_front();
      c_.write_evicted.fetch_add(1);
    }
    out_.push_back(std::move(o));
    set_out(out_.size());
  }

  void set_out(std::size_t n) {
    c_.out.store(n);
    if (n > c_.out_hw.load()) c_.out_hw.store(n);
  }

  // Worker thread only, for its one-line refusal while stopping.
  static void reply_close(int fd, const std::string& reply) {
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
      for (const Out& o : out_) pfds.push_back({o.fd, POLLOUT, 0});
      // Wake for the next read or write deadline, or to see stop_.
      int timeout = 100;
      const u64 now0 = mono_ns();
      auto until = [&](u64 dl) {
        const int ms = dl <= now0 ? 0 : static_cast<int>((dl - now0) / 1000000) + 1;
        if (ms < timeout) timeout = ms;
      };
      for (const Conn& c : half) until(c.accepted_ns + u64{kReadMs} * 1000000);
      for (const Out& o : out_) until(o.deadline_ns);
      int n = ::poll(pfds.data(), pfds.size(), timeout);
      if (n < 0) {
        // Never give up the port: a silent admin port reads as a dead
        // node and gets it failed over.
        if (errno != EINTR) {
          SNC_TRACE("admin_poll_fail errno=%d", errno);
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        continue;
      }
      const u64 now = mono_ns();
      // Waiting replies first, while pfds[1 + half.size() + i] is still
      // out_[i] (reading below may add to out_ and evict its oldest).
      {
        const std::size_t base = 1 + half.size();
        for (std::size_t i = 0; i < out_.size(); ++i) {
          Out& o = out_[i];
          if (pfds[base + i].revents != 0) {
            const Sent st = send_some(o);
            if (st != Sent::BLOCKED) {
              if (st == Sent::FAILED) c_.write_failed.fetch_add(1);
              ::close(o.fd);
              o.fd = -1;
              continue;
            }
          }
          if (now >= o.deadline_ns) {
            SNC_TRACE("admin_write_timeout sent=%zu of=%zu", o.off, o.buf.size());
            ::close(o.fd);
            o.fd = -1;
            c_.write_timeouts.fetch_add(1);
          }
        }
        for (auto it = out_.begin(); it != out_.end();) it = it->fd < 0 ? out_.erase(it) : it + 1;
        set_out(out_.size());
      }
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
    for (Out& o : out_) ::close(o.fd);  // stopping: unfinished replies end here
    out_.clear();
    set_out(0);
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
        reply(c.fd, "NO line too long", now);
        c.fd = -1;
        return;
      }
    }
  }

  void dispatch(int fd, const std::string& line, u64 accepted_ns) {
    std::string reply;
    if (h_.inline_verb && h_.inline_verb(line, reply)) {
      this->reply(fd, reply, mono_ns());
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
      this->reply(fd, "NO busy", mono_ns());
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
  int write_ms_;
  std::size_t out_max_;
  std::deque<Out> out_;  // loop thread only
  Counters c_;
  std::atomic<bool> stop_{false}, stopped_{false};
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> q_;
  bool worker_stop_ = false;
  std::thread loop_, worker_;
};

}  // namespace snc::admin
