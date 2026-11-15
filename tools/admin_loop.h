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
//
// Kept connections (K0, docs/CONTROL_CONNECTIONS_DESIGN.md 4.1 and 4.3),
// opt-in per connection: a connection whose first line is "KEEP 1" is
// answered "OK keep" and stays open. Each later request on it is
// "K <seq> <line>" and its reply "<seq> <reply>"; a kept connection holds
// one request at a time. Everything above is unchanged for a connection
// that does not send KEEP 1, which includes every HEALTH, STATUS and
// PROMOTE a witness sends. On a kept connection:
//   - a request's line is due within kReadMs of its first byte; an idle
//     one closes after idle_ms (30 s); idle ones are capped (idle_max,
//     least recently used closed first) and live in their own set, never
//     in the half-read set, and EMFILE never closes one;
//   - the worker and a MOVE thread write the reply with a deadline and hand
//     the connection back to the loop (an eventfd wakes it at once);
//   - a line that is not "K <digits> ...", or any byte after the request
//     line that is waiting when its reply is about to be sent, is a
//     protocol error: the connection closes (kept_proto, which also counts
//     a kept MOVE line not read by its deadline and a reply holding a
//     newline). A kept line over kLineMax closes without a reply.
// Keep off (--no-keep): KEEP is not special, so it goes to the worker as an
// unknown verb, exactly as on a server from before K0.
#pragma once

#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <list>
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
inline constexpr int kKeepIdleMs = 30000;  // design 4.3: the client closes at 20 s, first
inline constexpr std::size_t kKeepIdleCap = 64;

struct Counters {
  std::atomic<unsigned long long> queue_depth{0}, queue_hw{0}, busy{0};
  std::atomic<unsigned long long> half{0}, half_hw{0}, evicted{0}, timeouts{0};
  // S4.1: replies waiting for the client to read; closed unfinished at the
  // write deadline, on a send error, or to admit a newer one past kOutMax.
  std::atomic<unsigned long long> out{0}, out_hw{0}, write_timeouts{0}, write_failed{0}, write_evicted{0};
  // Failed accepts (EMFILE, ENFILE, ENOBUFS, ENOMEM), one per attempt: the
  // loop retries every pass, so this counts attempts, not connections.
  std::atomic<unsigned long long> accept_fail{0};
  // Kept connections (K0): open now (any state), handshakes accepted and
  // refused (cap), idle ones closed at idle_ms or evicted past idle_max,
  // and protocol errors.
  std::atomic<unsigned long long> kept{0}, keep_ok{0}, keep_full{0}, kept_idle_closed{0}, kept_evicted{0},
      kept_proto{0};
};

// Settings for kept connections. idle_max 0: the smaller of kKeepIdleCap
// and a sixteenth of RLIMIT_NOFILE (design 4.3).
struct KeepOptions {
  bool on = true;
  int idle_ms = kKeepIdleMs;
  std::size_t idle_max = 0;
};

inline std::size_t keep_idle_cap(std::size_t asked) {
  if (asked != 0) return asked;
  rlimit rl{};
  std::size_t cap = kKeepIdleCap;
  if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
    cap = std::min<std::size_t>(cap, static_cast<std::size_t>(rl.rlim_cur) / 16);
  }
  return cap == 0 ? 1 : cap;
}

// Writes all of `data` to fd by `deadline_ns` (mono_ns) without blocking
// past it: the kept-connection reply path of the worker and MOVE threads.
inline bool write_by(int fd, const std::string& data, u64 deadline_ns);

inline u64 mono_ns() {
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
}

// For a MOVE handler: completes the line whose first bytes are `got`
// (blocking fd), by `deadline_ns` (mono_ns). Keeps the line without its
// newline, as ha::read_line does. `strict` (a kept connection): a byte
// after the newline fails it, since the client sends one request at a
// time; without it they are dropped, as on a one-shot connection.
inline bool finish_line(int fd, std::string& got, u64 deadline_ns, bool strict = false) {
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
  if (strict && nl + 1 != got.size()) return false;
  got.resize(nl);
  return true;
}

inline bool write_by(int fd, const std::string& data, u64 deadline_ns) {
  std::size_t off = 0;
  while (off < data.size()) {
    const ssize_t r = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      const u64 now = mono_ns();
      if (now >= deadline_ns || !ha::wait_fd(fd, POLLOUT, static_cast<int>((deadline_ns - now) / 1000000) + 1)) {
        return false;
      }
      continue;
    }
    if (r <= 0) return false;
    off += static_cast<std::size_t>(r);
  }
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
    // MOVE: called with the connection (blocking mode) and the bytes read
    // so far, starting at "MOVE "; it must not block the loop (start a
    // thread). It reads the rest of the line itself (finish_line, passing
    // `strict`), then calls done(true, reply), or done(false, "") if the
    // line did not arrive. done writes the reply and closes the fd, or on a
    // kept connection writes "<seq> <reply>" and hands it back; after done
    // the handler must not touch fd. Null: MOVE goes to `heavy` like any
    // other verb.
    using MoveDone = std::function<void(bool line_ok, const std::string& reply)>;
    std::function<void(int fd, std::string got, u64 deadline_ns, bool strict, MoveDone done)> move;
  };

  // write_ms: the reply write deadline; out_max: replies that may wait
  // (tests lower both).
  Loop(int listen_fd, Handlers h, int write_ms = kWriteMs, std::size_t out_max = kOutMax, KeepOptions keep = {})
      : lfd_(listen_fd),
        h_(std::move(h)),
        write_ms_(write_ms),
        out_max_(out_max == 0 ? 1 : out_max),
        keep_(keep.on),
        idle_ms_(std::clamp(keep.idle_ms, 1, 3600 * 1000)),
        idle_max_(keep_idle_cap(keep.idle_max)) {
    ::fcntl(lfd_, F_SETFL, ::fcntl(lfd_, F_GETFL) | O_NONBLOCK);
    efd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    // Without it a handback waits for the loop's 100 ms tick: slower, not wrong.
    if (efd_ < 0) SNC_TRACE("admin_eventfd_fail errno=%d", errno);
  }
  // Starts answering. Separate from the constructor so the caller can
  // publish the Loop (its counters) before the first request.
  void start() {
    loop_ = std::thread([this] { run(); });
    worker_ = std::thread([this] { work(); });
  }
  ~Loop() {
    stop();
    if (efd_ >= 0) ::close(efd_);
  }

  // Stop accepting, close the half-read and kept connections, answer what
  // is queued "NO stopping", and join both threads. A heavy request already
  // running finishes first. MOVE threads are the caller's to wait for (a
  // kept one handed back after stop is closed); the Loop must outlive them.
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
  std::size_t keep_idle_max() const { return idle_max_; }

  // Tests only (DEBUG ACCEPT-HOLD, --debug-admin; docs/CONTROL_CONNECTIONS_DESIGN.md 5, A2): for `ms`, the
  // loop accepts nothing new while it keeps serving the connections it holds, as a stalled accept path
  // would. New connections wait in the kernel's backlog. 0 ends a hold.
  void debug_hold_accept(unsigned ms) {
    accept_hold_until_ns_.store(ms == 0 ? 0 : mono_ns() + u64{ms} * 1000000);
  }

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
    u64 kid = 0;      // a kept connection's id, 0 for one-shot
    std::string seq;  // its request's sequence number
  };

  struct Out {
    int fd;
    std::string buf;
    std::size_t off;
    u64 deadline_ns;
    u64 kid = 0;  // kept: back to idle when sent, instead of closed
  };

  // A kept connection. IDLE and READING are polled for input; SENDING is a
  // reply in out_; AWAY is with the worker or a MOVE thread, which owns the
  // fd until it hands it back.
  enum class KSt { IDLE, READING, SENDING, AWAY };
  struct Kept {
    int fd;
    u64 id;
    KSt st;
    u64 last_ns;  // entered IDLE (the idle deadline counts from here)
    u64 line_ns;  // first byte of the request being read
    std::string buf;
  };
  struct Back {
    u64 id;
    bool ok;
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
  void reply(int fd, const std::string& text, u64 now, u64 kid = 0) {
    Out o{fd, text + "\n", 0, now + static_cast<u64>(write_ms_) * 1000000, kid};
    switch (send_some(o)) {
      case Sent::DONE:
        if (kid != 0) {
          to_idle(kid, now);
        } else {
          ::close(fd);
        }
        return;
      case Sent::FAILED:
        close_out(o);
        c_.write_failed.fetch_add(1);
        return;
      case Sent::BLOCKED: break;
    }
    if (kid != 0) find_kept(kid)->st = KSt::SENDING;
    if (out_.size() >= out_max_) {
      close_out(out_.front());
      out_.pop_front();
      c_.write_evicted.fetch_add(1);
    }
    out_.push_back(std::move(o));
    set_out(out_.size());
  }

  // An unfinished reply ends here: its connection closes (a kept one is
  // forgotten too).
  void close_out(Out& o) {
    if (o.kid != 0) {
      drop_kept(o.kid);
    } else {
      ::close(o.fd);
    }
    o.fd = -1;
  }

  // ---- Kept connections (loop thread only, except handback) ----

  // Before a kept reply is sent: the client sends one request at a time,
  // so a byte already waiting on the connection (or its close) is a
  // protocol error (design 4.3). Bytes that arrive after the reply's last
  // byte has left cannot be told apart from the next request; the client's
  // sequence numbers cover those. Any thread.
  static bool stray(int fd) {
    pollfd p{fd, POLLIN, 0};
    int r;
    do r = ::poll(&p, 1, 0);
    while (r < 0 && errno == EINTR);
    return r != 0;
  }
  // "<seq> <reply>\n" for a kept connection; false if the reply holds a
  // newline, which no reply may (design 4.1): the connection is then
  // closed rather than send a line the client would split.
  bool kept_line(const std::string& seq, const std::string& reply, std::string& out) {
    if (reply.find('\n') != std::string::npos) {
      SNC_TRACE("admin_kept_newline bytes=%zu", reply.size());
      c_.kept_proto.fetch_add(1);
      return false;
    }
    out = seq + " " + reply + "\n";
    return true;
  }
  // The worker's and a MOVE thread's kept reply: checked, written by the
  // write deadline, then the connection handed back (or closed).
  void kept_reply(u64 kid, int fd, const std::string& seq, const std::string& reply) {
    std::string out;
    if (stray(fd)) {
      SNC_TRACE("admin_kept_proto why=%s", "bytes before the reply");
      c_.kept_proto.fetch_add(1);
      handback(kid, fd, false);
      return;
    }
    if (!kept_line(seq, reply, out)) {
      handback(kid, fd, false);
      return;
    }
    const bool sent = write_by(fd, out, mono_ns() + static_cast<u64>(write_ms_) * 1000000);
    if (!sent) c_.write_failed.fetch_add(1);
    handback(kid, fd, sent);
  }

  // Records are only marked closed (fd = -1) here; the loop erases them at
  // the top of a pass, so pointers it holds during a pass stay valid.
  Kept* find_kept(u64 id) {
    for (Kept& k : kept_) {
      if (k.id == id && k.fd >= 0) return &k;
    }
    return nullptr;
  }
  void set_kept() {
    std::size_t n = 0;
    for (const Kept& k : kept_) n += k.fd >= 0 ? 1 : 0;
    c_.kept.store(n);
  }
  // Closes a kept connection the loop holds.
  void drop_kept(u64 id) {
    if (Kept* k = find_kept(id)) {
      ::close(k->fd);
      k->fd = -1;
      set_kept();
    }
  }
  // A reply sent: the connection waits for its next request. Most recently
  // used last, so eviction takes from the front.
  void to_idle(u64 id, u64 now) {
    for (auto it = kept_.begin(); it != kept_.end(); ++it) {
      if (it->id != id || it->fd < 0) continue;
      it->st = KSt::IDLE;
      it->last_ns = now;
      it->buf.clear();
      kept_.splice(kept_.end(), kept_, it);
      cap_idle();
      return;
    }
  }
  // Past idle_max idle connections, close the least recently used.
  void cap_idle() {
    std::size_t idle = 0;
    for (const Kept& k : kept_) idle += k.st == KSt::IDLE && k.fd >= 0 ? 1 : 0;
    for (auto it = kept_.begin(); idle > idle_max_ && it != kept_.end(); ++it) {
      if (it->st != KSt::IDLE || it->fd < 0) continue;
      c_.kept_evicted.fetch_add(1);  // before the close a client can observe
      ::close(it->fd);
      it->fd = -1;
      --idle;
    }
    set_kept();
  }
  void proto_error(Kept& k, const char* why) {
    SNC_TRACE("admin_kept_proto why=%s bytes=%zu", why, k.buf.size());
    c_.kept_proto.fetch_add(1);  // before the close a client can observe
    ::close(k.fd);
    k.fd = -1;  // erased by the loop pass
  }

 public:
  // From the worker or a MOVE thread: the kept connection `id` is the
  // loop's again (ok), or failed and is closed (!ok). After stop, closed.
  void handback(u64 id, int fd, bool ok) {
    {
      std::lock_guard<std::mutex> g(hb_mu_);
      if (!hb_closed_) {
        hb_.push_back(Back{id, ok});
        const u64 one = 1;
        const ssize_t w = ::write(efd_, &one, sizeof(one));
        (void)w;
        return;
      }
    }
    ::close(fd);
  }

 private:
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
    std::vector<Kept*> kpoll;  // the kept connections in pfds, in order
    while (!stop_.load()) {
      for (auto it = kept_.begin(); it != kept_.end();) it = it->fd < 0 ? kept_.erase(it) : std::next(it);
      set_kept();
      pfds.clear();
      kpoll.clear();
      // A test's accept hold (debug_hold_accept): the listener stays in
      // pfds[0], so the indexes below hold, but is not polled.
      const u64 hold_ns = accept_hold_until_ns_.load();
      const bool holding = hold_ns != 0 && mono_ns() < hold_ns;
      pfds.push_back({lfd_, static_cast<short>(holding ? 0 : POLLIN), 0});
      for (const Conn& c : half) pfds.push_back({c.fd, POLLIN, 0});
      for (const Out& o : out_) pfds.push_back({o.fd, POLLOUT, 0});
      const std::size_t kbase = pfds.size();
      for (Kept& k : kept_) {
        if (k.fd < 0 || (k.st != KSt::IDLE && k.st != KSt::READING)) continue;
        pfds.push_back({k.fd, POLLIN, 0});
        kpoll.push_back(&k);
      }
      const std::size_t ebase = pfds.size();
      pfds.push_back({efd_, POLLIN, 0});
      // Wake for the next read or write deadline, or to see stop_.
      int timeout = 100;
      const u64 now0 = mono_ns();
      auto until = [&](u64 dl) {
        const int ms = dl <= now0 ? 0 : static_cast<int>((dl - now0) / 1000000) + 1;
        if (ms < timeout) timeout = ms;
      };
      for (const Conn& c : half) until(c.accepted_ns + u64{kReadMs} * 1000000);
      for (const Out& o : out_) until(o.deadline_ns);
      for (const Kept* k : kpoll) {
        until(k->st == KSt::READING ? k->line_ns + u64{kReadMs} * 1000000
                                    : k->last_ns + static_cast<u64>(idle_ms_) * 1000000);
      }
      if (holding) until(hold_ns);
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
            if (o.kid != 0 && stray(o.fd)) {  // bytes before the reply was all sent
              SNC_TRACE("admin_kept_proto why=%s", "bytes before the reply");
              c_.kept_proto.fetch_add(1);
              close_out(o);
              continue;
            }
            const Sent st = send_some(o);
            if (st == Sent::DONE && o.kid != 0) {
              to_idle(o.kid, now);
              o.fd = -1;
              continue;
            }
            if (st != Sent::BLOCKED) {
              if (st == Sent::FAILED) c_.write_failed.fetch_add(1);
              close_out(o);
              continue;
            }
          }
          if (now >= o.deadline_ns) {
            SNC_TRACE("admin_write_timeout sent=%zu of=%zu", o.off, o.buf.size());
            close_out(o);
            c_.write_timeouts.fetch_add(1);
          }
        }
        for (auto it = out_.begin(); it != out_.end();) it = it->fd < 0 ? out_.erase(it) : it + 1;
        set_out(out_.size());
      }
      // Kept connections: what arrived, then their deadlines. The Kept
      // records stay in place during this (std::list), so kpoll is valid;
      // closed ones are marked fd = -1 and erased after.
      {
        for (std::size_t i = 0; i < kpoll.size(); ++i) {
          Kept& k = *kpoll[i];
          // Still waiting for input (a pass may have evicted it, or a
          // request taken it AWAY or SENDING).
          if (pfds[kbase + i].revents != 0 && k.fd >= 0 && (k.st == KSt::IDLE || k.st == KSt::READING)) {
            on_kept_readable(k, now);
          }
        }
        for (Kept* kp : kpoll) {
          Kept& k = *kp;
          if (k.fd < 0 || (k.st != KSt::IDLE && k.st != KSt::READING)) continue;
          // (last_ns may be later than this pass's now: a reply sent during
          // the pass stamps mono_ns().)
          if (k.st == KSt::READING && now >= k.line_ns && now - k.line_ns >= u64{kReadMs} * 1000000) {
            SNC_TRACE("admin_read_fail kept read_ms=%.1f bytes=%zu", (now - k.line_ns) / 1e6, k.buf.size());
            c_.timeouts.fetch_add(1);
            ::close(k.fd);
            k.fd = -1;
          } else if (k.st == KSt::IDLE && now >= k.last_ns &&
                     now - k.last_ns >= static_cast<u64>(idle_ms_) * 1000000) {
            c_.kept_idle_closed.fetch_add(1);
            ::close(k.fd);
            k.fd = -1;
          }
        }
        set_kept();
      }
      // Handed back by the worker or a MOVE thread.
      if (pfds[ebase].revents != 0) {
        u64 v;
        while (::read(efd_, &v, sizeof(v)) > 0) {
        }
      }
      {
        std::vector<Back> backs;
        {
          std::lock_guard<std::mutex> g(hb_mu_);
          backs.swap(hb_);
        }
        for (const Back& b : backs) {
          Kept* k = find_kept(b.id);
          if (k == nullptr) continue;
          if (!b.ok) {
            drop_kept(b.id);
            continue;
          }
          ::fcntl(k->fd, F_SETFL, ::fcntl(k->fd, F_GETFL) | O_NONBLOCK);
          to_idle(b.id, now);
        }
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
      // (A hold set while poll waited takes effect here too.)
      const u64 hold_now = accept_hold_until_ns_.load();
      if (n > 0 && (pfds[0].revents & POLLIN) && !(hold_now != 0 && mono_ns() < hold_now)) {
        while (true) {
          int fd = ::accept4(lfd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
          if (fd < 0) {
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
              c_.accept_fail.fetch_add(1);
              // Out of descriptors: the listener stays readable, so free
              // one (the oldest half-read connection) or back off rather
              // than spin. Kept connections are never closed for this.
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
    for (Out& o : out_) {
      if (o.kid == 0) ::close(o.fd);  // stopping: unfinished replies end here
    }
    out_.clear();
    set_out(0);
    {
      // From here a handback closes its own fd. One already queued is the
      // loop's: closed below with the rest the loop holds.
      std::lock_guard<std::mutex> g(hb_mu_);
      hb_closed_ = true;
      for (const Back& b : hb_) {
        if (Kept* k = find_kept(b.id)) k->st = KSt::IDLE;
      }
      hb_.clear();
    }
    for (Kept& k : kept_) {
      if (k.fd >= 0 && k.st != KSt::AWAY) ::close(k.fd);
    }
    kept_.clear();
    set_kept();
    ::close(lfd_);
  }

  // A kept connection has input: the start or the rest of a request
  // "K <seq> <line>", or EOF.
  void on_kept_readable(Kept& k, u64 now) {
    char buf[16384];
    while (true) {
      const ssize_t r = ::recv(k.fd, buf, sizeof(buf), 0);
      if (r < 0 && errno == EINTR) continue;
      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      if (r <= 0) {  // the client closed it (idle) or it failed
        if (k.st == KSt::READING) {
          SNC_TRACE("admin_read_fail kept read_ms=%.1f bytes=%zu", (now - k.line_ns) / 1e6, k.buf.size());
        }
        ::close(k.fd);
        k.fd = -1;
        return;
      }
      if (k.st == KSt::IDLE) {
        k.st = KSt::READING;
        k.line_ns = now;
      }
      k.buf.append(buf, static_cast<std::size_t>(r));
      // "K <seq> " first.
      if (k.buf[0] != 'K' || (k.buf.size() >= 2 && k.buf[1] != ' ')) return proto_error(k, "prefix");
      const std::size_t sp = k.buf.find(' ', 2);
      if (sp == std::string::npos) {
        if (k.buf.size() > 2 + 20) return proto_error(k, "seq");
        for (std::size_t i = 2; i < k.buf.size(); ++i) {
          if (k.buf[i] < '0' || k.buf[i] > '9') return proto_error(k, "seq");
        }
        continue;
      }
      if (sp == 2 || sp > 2 + 20) return proto_error(k, "seq");
      for (std::size_t i = 2; i < sp; ++i) {
        if (k.buf[i] < '0' || k.buf[i] > '9') return proto_error(k, "seq");
      }
      const std::size_t at = sp + 1;
      if (h_.move && k.buf.size() >= at + 5 && k.buf.compare(at, 5, "MOVE ") == 0) {
        // As on a one-shot connection: the move thread reads the rest.
        std::string seq = k.buf.substr(2, sp - 2);
        std::string got = k.buf.substr(at);
        k.buf.clear();
        k.st = KSt::AWAY;
        ::fcntl(k.fd, F_SETFL, ::fcntl(k.fd, F_GETFL) & ~O_NONBLOCK);
        const int fd = k.fd;
        const u64 id = k.id;
        h_.move(fd, std::move(got), k.line_ns + u64{kReadMs} * 1000000, true,
                [this, fd, id, seq = std::move(seq)](bool line_ok, const std::string& out) {
                  if (!line_ok) {
                    // Not read by its deadline, closed, or bytes after the
                    // line (strict): counted with the protocol errors.
                    c_.kept_proto.fetch_add(1);
                    handback(id, fd, false);
                    return;
                  }
                  kept_reply(id, fd, seq, out);
                });
        return;
      }
      const std::size_t nl = k.buf.find('\n', at);
      if (nl == std::string::npos) {
        if (k.buf.size() > kLineMax) return proto_error(k, "too long");
        continue;
      }
      // One request at a time: nothing may follow its line.
      if (nl + 1 != k.buf.size()) return proto_error(k, "bytes after the line");
      std::string seq = k.buf.substr(2, sp - 2);
      std::string line = k.buf.substr(at, nl - at);
      k.buf.clear();
      dispatch(k.fd, line, k.line_ns, k.id, std::move(seq));
      return;
    }
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
        h_.move(fd, std::move(c.buf), c.accepted_ns + u64{kReadMs} * 1000000, false,
                [fd](bool line_ok, const std::string& out) {
                  if (line_ok) ha::write_all(fd, out + "\n");
                  ::close(fd);
                });
        return;
      }
      const auto nl = c.buf.find('\n');
      if (nl != std::string::npos) {
        if (keep_ && nl == 6 && c.buf.compare(0, 7, "KEEP 1\n") == 0) {
          keep(c, now);
          return;
        }
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

  // "KEEP 1" as a connection's first line: answer "OK keep" and keep it
  // (inline, never queued). Refused past twice the idle cap of kept
  // connections in any state. The client waits for "OK keep" before its
  // first request, so a byte after the line is a protocol error.
  void keep(Conn& c, u64 now) {
    const int fd = c.fd;
    c.fd = -1;
    if (c.buf.size() != 7) {
      c_.kept_proto.fetch_add(1);
      ::close(fd);
      return;
    }
    std::size_t live = 0;
    for (const Kept& k : kept_) live += k.fd >= 0 ? 1 : 0;
    if (live >= 2 * idle_max_) {
      c_.keep_full.fetch_add(1);
      reply(fd, "NO keep full", now);
      return;
    }
    const u64 id = ++next_kid_;
    kept_.push_back(Kept{fd, id, KSt::SENDING, now, 0, {}});
    set_kept();
    c_.keep_ok.fetch_add(1);
    reply(fd, "OK keep", now, id);
  }

  void dispatch(int fd, const std::string& line, u64 accepted_ns, u64 kid = 0, std::string seq = {}) {
    std::string reply;
    const auto text = [&](const std::string& r) { return kid == 0 ? r : seq + " " + r; };
    // A kept reply from the loop: its checks first (see kept_reply).
    const auto kept_ok = [&](const std::string& r) {
      if (kid == 0) return true;
      std::string unused;
      if (stray(fd)) {
        SNC_TRACE("admin_kept_proto why=%s", "bytes before the reply");
        c_.kept_proto.fetch_add(1);
      } else if (kept_line(seq, r, unused)) {
        return true;
      }
      drop_kept(kid);
      return false;
    };
    if (h_.inline_verb && h_.inline_verb(line, reply)) {
      if (!kept_ok(reply)) return;
      this->reply(fd, text(reply), mono_ns(), kid);
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
      if (!kept_ok("NO busy")) return;
      this->reply(fd, text("NO busy"), mono_ns(), kid);
      return;
    }
    if (kid != 0) find_kept(kid)->st = KSt::AWAY;
    q_.push_back(Job{fd, line, accepted_ns, kid, std::move(seq)});
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
          if (j.kid != 0) {
            write_by(j.fd, j.seq + " NO stopping\n", mono_ns());
            handback(j.kid, j.fd, false);
          } else {
            reply_close(j.fd, "NO stopping");
          }
          continue;
        }
      }
      const u64 t_run = mono_ns();
      const std::string reply = h_.heavy(j.line);
      if (j.kid != 0) {
        kept_reply(j.kid, j.fd, j.seq, reply);  // with a deadline, then the loop's again
      } else {
        ::fcntl(j.fd, F_SETFL, ::fcntl(j.fd, F_GETFL) & ~O_NONBLOCK);
        ha::write_all(j.fd, reply + "\n");
        ::close(j.fd);
      }
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
  bool keep_;
  int idle_ms_;
  std::size_t idle_max_;
  std::list<Kept> kept_;  // loop thread only; least recently idle first
  u64 next_kid_ = 0;
  int efd_ = -1;
  std::mutex hb_mu_;
  std::vector<Back> hb_;
  bool hb_closed_ = false;
  Counters c_;
  std::atomic<bool> stop_{false}, stopped_{false};
  std::atomic<u64> accept_hold_until_ns_{0};
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> q_;
  bool worker_stop_ = false;
  std::thread loop_, worker_;
};

}  // namespace snc::admin
