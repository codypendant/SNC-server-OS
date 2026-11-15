// S4.1: admin replies are complete or explicitly cut, and never hold the
// loop (docs/S4_SYNC_TOKENS.md section 9; work order
// docs/handoff/PR81_S4_Review.txt section 6).
//
// A real admin::Loop on a loopback port, with an inline verb "BIG n" that
// answers n bytes (GAUGES is an inline verb of the same kind) and HEALTH.
//
//   A1 slow reader: a 16 MiB reply to a client with a 4 KiB receive buffer
//      that reads slowly is not taken by one send; it is delivered whole,
//      ending in its newline, and HEALTH answers promptly meanwhile
//   A2 stalled reader: a client that never reads is closed at the write
//      deadline; what it then reads ends without the newline (the cut is
//      visible), write_timeouts counts it, HEALTH answered promptly while
//      it waited, and nothing is left waiting
//   A3 bound: past the limit of waiting replies (4 here, kOutMax = 64 by
//      default), the oldest is closed and
//      counted (write_evicted); HEALTH still answers
//   A4 a reply the socket takes at once (HEALTH) leaves nothing waiting
//      (a regression guard; it also passes with a one-call send)
//
// HEALTH baseline (docs/CONTROL_CONNECTIONS_DESIGN.md 5, A6, and section 6):
//   H1 heavy worker stalled, its queue full (the next heavy request gets
//      "NO busy") and 200 connections that never send a line: a new
//      one-shot HEALTH is answered inline, promptly, every time
//   H2 accept hold (DEBUG ACCEPT-HOLD's loop side): a connection accepted
//      before the hold is still answered during it; a new connection's
//      HEALTH waits in the backlog until the hold ends, then is answered
//
// K0 kept connections (docs/CONTROL_CONNECTIONS_DESIGN.md 4.1 to 4.3; A4,
// A6 with the idle set populated):
//   K1 handshake, then inline, heavy and MOVE requests on one connection,
//      each reply carrying its sequence number; one-shot MOVE unchanged
//   K2 protocol errors close the connection, and the next connection gets
//      its own replies: bytes after a request line, a line without
//      "K <seq> ", a bad sequence number, bytes after "KEEP 1"
//   K3 deadlines: a request line is due kReadMs after its first byte; an
//      idle connection closes at idle_ms
//   K4 idle cap: past idle_max the least recently used idle connection is
//      closed; past twice the cap a handshake is refused "NO keep full"
//   K5 (A6) the idle set full, the heavy worker stalled, its queue full and
//      200 half-read connections: one-shot HEALTH still answered inline
//   K6 keep off (--no-keep): KEEP 1 is an unknown heavy verb, as before K0
//   K7 stop() with kept connections idle and one with the worker returns,
//      and the worker's handback after it closes the connection
// ha::Conn, the client (design 4.2), against a scripted server:
//   C1 the first request goes one-shot while the handshake runs; later ones
//      share one kept connection
//   C2 a server that refuses KEEP: every request one-shot, never a "K "
//      line, one handshake attempt per 60 s
//   C3 a wrong sequence number or bytes after the reply: false, closed
//      (RST), and the next request gets its own reply
//   C4 the server closes an idle connection: the reuse check sees it and
//      the request goes one-shot
//   C5 the server closes after reading a request (no reply byte) on a
//      reused connection: a read-only verb is resent once one-shot; any
//      other verb returns false and is never resent
//   C6 a reply late past the deadline: false, and it is never returned to
//      the next request
//   C7 SNC_HA_KEEP=0 (run by ctest as "keep-off"): no KEEP ever sent
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../tools/admin_loop.h"
#include "harness.h"

using namespace snc;

namespace {

constexpr int kWriteDeadlineMs = 400;

int listen_any(u16& port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  CHECK(fd >= 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
  CHECK_EQ(::listen(fd, 512), 0);
  socklen_t len = sizeof(a);
  CHECK_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len), 0);
  port = ntohs(a.sin_port);
  return fd;
}

int connect_to(u16 port, int rcvbuf = 0) {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  CHECK(fd >= 0);
  if (rcvbuf != 0) CHECK_EQ(::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)), 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(port);
  CHECK_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
  return fd;
}

void send_line(int fd, const std::string& line) { CHECK(ha::write_all(fd, line + "\n")); }

// Everything until EOF (bounded), `pause_us` between reads.
std::string read_all(int fd, int pause_us, std::size_t chunk) {
  std::string out;
  std::vector<char> b(chunk);
  while (true) {
    const ssize_t r = ::recv(fd, b.data(), b.size(), 0);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    out.append(b.data(), static_cast<std::size_t>(r));
    if (pause_us != 0) ::usleep(pause_us);
  }
  return out;
}

// HEALTH round trip in ms (fails if the reply is not exactly "OK\n").
double health_ms(u16 port) {
  const auto t0 = std::chrono::steady_clock::now();
  const int fd = connect_to(port);
  // Bounded: a loop held by a writer fails here instead of hanging.
  timeval tv{3, 0};
  CHECK_EQ(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), 0);
  send_line(fd, "HEALTH");
  const std::string r = read_all(fd, 0, 4096);
  ::close(fd);
  CHECK(r == "OK\n");
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

struct Server {
  u16 port = 0;
  std::unique_ptr<admin::Loop> loop;
  std::atomic<int> moves{0};  // MOVE threads still running
  explicit Server(int write_ms = kWriteDeadlineMs, std::size_t out_max = admin::kOutMax,
                  std::function<std::string(const std::string&)> heavy = nullptr, admin::KeepOptions keep = {}) {
    const int lfd = listen_any(port);
    admin::Loop::Handlers h;
    h.inline_verb = [](const std::string& line, std::string& reply) {
      if (line == "HEALTH") {
        reply = "OK";
        return true;
      }
      if (line == "NL") {  // a reply with a newline in it (no real verb has one)
        reply = "a\nb";
        return true;
      }
      if (line.rfind("BIG ", 0) == 0) {
        const std::size_t n = std::stoull(line.substr(4));
        reply.assign(n, 'g');
        reply[0] = 'G';  // a start marker; the end is the newline
        return true;
      }
      return false;
    };
    h.heavy = heavy ? std::move(heavy) : [](const std::string& line) { return "HEAVY " + line; };
    // As snc_server's: a thread reads the rest of the line and answers.
    h.move = [this](int fd, std::string got, u64 deadline, bool strict, admin::Loop::Handlers::MoveDone done) {
      moves.fetch_add(1);
      std::thread([this, fd, got = std::move(got), deadline, strict, done = std::move(done)]() mutable {
        if (admin::finish_line(fd, got, deadline, strict)) {
          done(true, "MOVED " + got);
        } else {
          done(false, "");
        }
        moves.fetch_sub(1);
      }).detach();
    };
    loop = std::make_unique<admin::Loop>(lfd, std::move(h), write_ms, out_max, keep);
    loop->start();
  }
  ~Server() {
    loop->stop();
    while (moves.load() != 0) ::usleep(1000);  // the Loop outlives its MOVE threads
  }
  const admin::Counters& c() const { return loop->counters(); }
};

void wait_for(const std::function<bool()>& p, const char* what) {
  for (int i = 0; i < 20000; ++i) {
    if (p()) return;
    ::usleep(500);
  }
  std::fprintf(stderr, "timed out: %s\n", what);
  CHECK(false);
}

void test_slow_reader() {
  // A long deadline: this test is about delivery to a reader that reads;
  // the deadline is A2's. (Under TSan 16 MiB through 4 KiB takes ~0.6 s,
  // past A2's 400 ms; S4.1 independent review.)
  Server s(30000);
  constexpr std::size_t kBig = 16u << 20;
  const int fd = connect_to(s.port, 4096);
  send_line(fd, "BIG " + std::to_string(kBig));
  // No wait on the loop's counters here: what is checked is what the
  // client receives (a one-call send must fail on the content below).
  ::usleep(50000);
  // HEALTH while the big reply waits.
  double worst = 0;
  for (int i = 0; i < 20; ++i) worst = std::max(worst, health_ms(s.port));
  // Read it all, slowly at first (small reads with pauses), then normally.
  std::string got;
  {
    std::vector<char> b(4096);
    for (int i = 0; i < 200; ++i) {
      const ssize_t r = ::recv(fd, b.data(), b.size(), 0);
      CHECK(r > 0);
      got.append(b.data(), static_cast<std::size_t>(r));
      ::usleep(500);
    }
  }
  got += read_all(fd, 0, 1 << 16);
  ::close(fd);
  CHECK_EQ(s.c().write_timeouts.load(), 0u);
  CHECK_EQ(got.size(), kBig + 1);
  CHECK(got.front() == 'G');
  CHECK(got.back() == '\n');
  CHECK_EQ(got.find('\n'), kBig);
  CHECK_EQ(s.c().write_timeouts.load(), 0u);
  CHECK_EQ(s.c().write_failed.load(), 0u);
  wait_for([&] { return s.c().out.load() == 0; }, "nothing left waiting");
  CHECK(s.c().out_hw.load() >= 1);
  CHECK(worst < 250.0);
  std::printf("A1 slow reader: 16 MiB reply waited for its 4 KiB-buffer reader and arrived whole with its newline; "
              "20 HEALTH meanwhile, worst %.1f ms\n",
              worst);
}

void test_stalled_reader() {
  Server s;
  constexpr std::size_t kBig = 16u << 20;
  const int fd = connect_to(s.port, 4096);
  send_line(fd, "BIG " + std::to_string(kBig));
  wait_for([&] { return s.c().out.load() == 1; }, "the reply waiting");
  double worst = 0;
  for (int i = 0; i < 20; ++i) worst = std::max(worst, health_ms(s.port));
  // Never read before the deadline.
  wait_for([&] { return s.c().write_timeouts.load() == 1; }, "the write deadline");
  // The count is published in the same loop pass, just after.
  wait_for([&] { return s.c().out.load() == 0; }, "nothing left waiting");
  const std::string got = read_all(fd, 0, 1 << 16);
  ::close(fd);
  CHECK(got.size() < kBig + 1);
  CHECK(got.empty() || got.back() != '\n');  // the cut is visible
  CHECK(got.find('\n') == std::string::npos);
  CHECK(worst < 250.0);
  std::printf("A2 stalled reader: closed at the %d ms write deadline after %zu of %zu bytes, no newline "
              "(write_timeouts=1); 20 HEALTH meanwhile, worst %.1f ms\n",
              kWriteDeadlineMs, got.size(), kBig + 1, worst);
}

void test_bound() {
  constexpr std::size_t kMax = 4;
  Server s(60000, kMax);  // no deadline during the test; a small bound keeps memory low
  constexpr std::size_t kBig = 16u << 20;
  std::vector<int> fds;
  for (std::size_t i = 0; i < kMax + 1; ++i) {
    const int fd = connect_to(s.port, 4096);
    send_line(fd, "BIG " + std::to_string(kBig));
    fds.push_back(fd);
    wait_for([&] { return s.c().out.load() + s.c().write_evicted.load() == i + 1; }, "each reply waiting");
  }
  CHECK_EQ(s.c().out.load(), kMax);
  CHECK_EQ(s.c().write_evicted.load(), 1u);
  const double ms = health_ms(s.port);
  CHECK(ms < 250.0);
  for (int fd : fds) ::close(fd);
  // Clients closing with unread data reset the connection: each waiting
  // reply then fails its next send and is closed (write_failed).
  wait_for([&] { return s.c().out.load() == 0; }, "all closed (deadline or error)");
  CHECK_EQ(s.c().write_failed.load() + s.c().write_timeouts.load(), kMax);
  std::printf("A3 bound: %zu waiting replies, one more closed the oldest (write_evicted=1); HEALTH %.1f ms\n",
              kMax, ms);
}

void test_small() {
  Server s;
  for (int i = 0; i < 50; ++i) health_ms(s.port);
  CHECK_EQ(s.c().out_hw.load(), 0u);
  CHECK_EQ(s.c().write_failed.load(), 0u);
  std::printf("A4 small replies: 50 HEALTH sent at once, none waited\n");
}

// One line from fd (blocking, 3 s receive timeout), without its newline.
std::string reply_of(int fd) {
  timeval tv{3, 0};
  CHECK_EQ(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), 0);
  std::string r = read_all(fd, 0, 4096);
  if (!r.empty() && r.back() == '\n') r.pop_back();
  return r;
}

void test_heavy_stalled() {
  std::atomic<bool> release{false};
  std::atomic<int> started{0};
  Server s(kWriteDeadlineMs, admin::kOutMax, [&](const std::string& line) {
    started.fetch_add(1);
    while (!release.load()) ::usleep(1000);
    return "HEAVY " + line;
  });
  // One request held on the worker, then kQueueMax queued behind it.
  std::vector<int> heavy;
  heavy.push_back(connect_to(s.port));
  send_line(heavy.back(), "STATUS");
  wait_for([&] { return started.load() == 1; }, "the worker holds a request");
  for (std::size_t i = 0; i < admin::kQueueMax; ++i) {
    heavy.push_back(connect_to(s.port));
    send_line(heavy.back(), "STATUS");
  }
  wait_for([&] { return s.c().queue_depth.load() == admin::kQueueMax; }, "the queue full");
  const int over = connect_to(s.port);
  send_line(over, "STATUS");
  CHECK(reply_of(over) == "NO busy");
  ::close(over);
  // Connections that never send a line, under the half-read bound.
  std::vector<int> idle;
  for (int i = 0; i < 200; ++i) idle.push_back(connect_to(s.port));
  wait_for([&] { return s.c().half.load() >= 200; }, "200 half-read connections");
  double worst = 0;
  for (int i = 0; i < 20; ++i) worst = std::max(worst, health_ms(s.port));
  CHECK(s.c().half.load() >= 200);  // still held (kReadMs is 500 ms): the probes ran under them
  CHECK_EQ(s.c().busy.load(), 1u);
  CHECK_EQ(s.c().queue_depth.load(), admin::kQueueMax);
  release.store(true);
  for (int fd : heavy) {
    CHECK(reply_of(fd) == "HEAVY STATUS");
    ::close(fd);
  }
  for (int fd : idle) ::close(fd);
  // Inline means not behind the held request; a generous bound for loaded
  // hosts (the loop wakes at least every 100 ms).
  CHECK(worst < 500);
  std::printf("H1 heavy worker stalled, queue full (NO busy past it), 200 half-read: 20 HEALTH inline, "
              "worst %.1f ms\n", worst);
}

void test_accept_hold() {
  Server s;
  // Accepted before the hold: a half-read connection the loop holds.
  const int early = connect_to(s.port);
  wait_for([&] { return s.c().half.load() == 1; }, "the early connection accepted");
  constexpr unsigned kHoldMs = 600;
  const auto t0 = std::chrono::steady_clock::now();
  s.loop->debug_hold_accept(kHoldMs);
  // A new connection completes in the kernel (the listen backlog) but is
  // not accepted: its HEALTH waits.
  const int late = connect_to(s.port);
  send_line(late, "HEALTH");
  send_line(early, "HEALTH");
  CHECK(reply_of(early) == "OK");
  const double early_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  ::close(early);
  CHECK(reply_of(late) == "OK");
  const double late_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  ::close(late);
  CHECK(early_ms < kHoldMs);
  CHECK(late_ms >= kHoldMs - 5);  // the hold is measured from before the connect
  // 0 ends a hold at once.
  s.loop->debug_hold_accept(60000);
  const int late2 = connect_to(s.port);
  send_line(late2, "HEALTH");
  ::usleep(200 * 1000);
  s.loop->debug_hold_accept(0);
  CHECK(reply_of(late2) == "OK");
  ::close(late2);
  CHECK(health_ms(s.port) < 500);
  std::printf("H2 accept hold %u ms: the connection accepted before it answered in %.1f ms, a new one in "
              "%.1f ms (after the hold); a hold of 60 s ended by 0\n", kHoldMs, early_ms, late_ms);
}


// ---- K0: kept connections, server side ----

// One line from fd, read a byte at a time (so nothing after it is taken),
// without its newline; "<EOF>" if the connection ended first.
std::string line_of(int fd) {
  timeval tv{3, 0};
  CHECK_EQ(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), 0);
  std::string s;
  char ch;
  while (true) {
    const ssize_t r = ::recv(fd, &ch, 1, 0);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return "<EOF>";
    if (ch == '\n') return s;
    s += ch;
  }
}

// True once the peer has closed fd (EOF or reset within 3 s).
bool closed_by_peer(int fd) {
  timeval tv{3, 0};
  CHECK_EQ(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), 0);
  char b[256];
  while (true) {
    const ssize_t r = ::recv(fd, b, sizeof(b), 0);
    if (r < 0 && errno == EINTR) continue;
    if (r > 0) continue;  // whatever was sent before the close
    return r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK);
  }
}

int keep_conn(u16 port) {
  const int fd = connect_to(port);
  send_line(fd, "KEEP 1");
  CHECK(line_of(fd) == "OK keep");
  return fd;
}

void test_keep_requests() {
  Server s;
  const int fd = keep_conn(s.port);
  wait_for([&] { return s.c().kept.load() == 1; }, "one kept connection");
  send_line(fd, "K 1 HEALTH");
  CHECK(line_of(fd) == "1 OK");
  send_line(fd, "K 2 STATUS");
  CHECK(line_of(fd) == "2 HEAVY STATUS");
  // MOVE is still recognized by its prefix after "K <seq> " (hazard 3).
  send_line(fd, "K 3 MOVE INFO 7");
  CHECK(line_of(fd) == "3 MOVED MOVE INFO 7");
  send_line(fd, "K 4 BIG 100000");
  const std::string big = line_of(fd);
  CHECK(big.size() == 2 + 100000 && big.compare(0, 3, "4 G") == 0);
  send_line(fd, "K 5 HEALTH");
  CHECK(line_of(fd) == "5 OK");
  CHECK_EQ(s.c().keep_ok.load(), 1u);
  CHECK_EQ(s.c().kept_proto.load(), 0u);
  ::close(fd);
  wait_for([&] { return s.c().kept.load() == 0; }, "the client's close seen");
  // One-shot MOVE: answered and closed, as before.
  const int one = connect_to(s.port);
  send_line(one, "MOVE INFO 9");
  CHECK(reply_of(one) == "MOVED MOVE INFO 9");
  ::close(one);
  std::printf("K1 kept: inline, heavy, MOVE and a 100 KB reply on one connection, each with its sequence "
              "number; one-shot MOVE unchanged\n");
}

void test_keep_proto() {
  Server s;
  struct Case {
    const char* name;
    std::string bytes;
  };
  const std::vector<Case> cases = {
      {"bytes after the line", "K 1 HEALTH\nK 2 HEALTH\n"},
      {"no K prefix", "HEALTH\n"},
      {"bad sequence number", "K x1 HEALTH\n"},
      {"empty sequence number", "K  HEALTH\n"},
  };
  unsigned long long want = 0;
  for (const Case& c : cases) {
    const int fd = keep_conn(s.port);
    CHECK(ha::write_all(fd, c.bytes));
    CHECK(closed_by_peer(fd));
    ::close(fd);
    ++want;
    wait_for([&] { return s.c().kept_proto.load() == want; }, c.name);
    // The next connection gets its own replies.
    const int next = keep_conn(s.port);
    send_line(next, "K 9 HEALTH");
    CHECK(line_of(next) == "9 OK");
    ::close(next);
  }
  // A request sent with KEEP 1, before "OK keep": closed.
  const int fd = connect_to(s.port);
  CHECK(ha::write_all(fd, "KEEP 1\nK 1 HEALTH\n"));
  CHECK(closed_by_peer(fd));
  ::close(fd);
  wait_for([&] { return s.c().kept_proto.load() == want + 1; }, "bytes after KEEP 1");
  // Bytes after a strict MOVE line (the MOVE thread reads it).
  {
    const int m = keep_conn(s.port);
    CHECK(ha::write_all(m, "K 1 MOVE INFO 7\nX"));
    CHECK(closed_by_peer(m));
    ::close(m);
    wait_for([&] { return s.c().kept_proto.load() == want + 2; }, "bytes after a MOVE line");
  }
  // A reply holding a newline: never sent on a kept connection (closed);
  // one-shot unchanged.
  {
    const int n = keep_conn(s.port);
    send_line(n, "K 1 NL");
    CHECK(closed_by_peer(n));
    ::close(n);
    wait_for([&] { return s.c().kept_proto.load() == want + 3; }, "a reply with a newline");
    const int o = connect_to(s.port);
    send_line(o, "NL");
    CHECK(reply_of(o) == "a\nb");
    ::close(o);
  }
  CHECK(health_ms(s.port) < 500);
  // A second request sent while the first is with the worker: closed
  // before the first reply is written.
  {
    std::atomic<bool> release{false};
    std::atomic<int> started{0};
    Server h(kWriteDeadlineMs, admin::kOutMax, [&](const std::string& line) {
      started.fetch_add(1);
      while (!release.load()) ::usleep(1000);
      return "HEAVY " + line;
    });
    const int fd = keep_conn(h.port);
    send_line(fd, "K 1 STATUS");
    wait_for([&] { return started.load() == 1; }, "the worker holds the kept request");
    send_line(fd, "K 2 HEALTH");
    ::usleep(50 * 1000);
    release.store(true);
    CHECK(line_of(fd) == "<EOF>");  // no reply to either
    ::close(fd);
    wait_for([&] { return h.c().kept_proto.load() == 1; }, "bytes while with the worker");
  }
  std::printf("K2 protocol errors (%zu kinds, bytes after KEEP 1, after a MOVE line, a second request while the "
              "first is with the worker, a reply holding a newline) close the connection; the next one gets "
              "its own replies\n", cases.size());
}

void test_keep_deadlines() {
  admin::KeepOptions k;
  k.idle_ms = 300;
  Server s(kWriteDeadlineMs, admin::kOutMax, nullptr, k);
  // A request started but not finished: closed kReadMs after its first byte.
  const int fd = keep_conn(s.port);
  ::usleep(200 * 1000);  // idle first: the request deadline counts from its first byte, not the accept
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(ha::write_all(fd, "K 1 HEA"));
  CHECK(closed_by_peer(fd));
  const double read_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  ::close(fd);
  CHECK(read_ms >= admin::kReadMs - 5 && read_ms < admin::kReadMs + 1500);  // upper: loaded or sanitized hosts
  // Idle: closed at idle_ms after its last reply.
  const int idle = keep_conn(s.port);
  send_line(idle, "K 1 HEALTH");
  CHECK(line_of(idle) == "1 OK");
  const auto t1 = std::chrono::steady_clock::now();
  CHECK(closed_by_peer(idle));
  const double idle_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
  ::close(idle);
  CHECK(idle_ms >= 300 - 5 && idle_ms < 300 + 1500);
  CHECK_EQ(s.c().kept_idle_closed.load(), 1u);
  CHECK(s.c().timeouts.load() >= 1u);
  std::printf("K3 deadlines: a partial request closed %.0f ms after its first byte (kReadMs %d), an idle "
              "connection %.0f ms after its reply (idle_ms 300)\n", read_ms, admin::kReadMs, idle_ms);
}

void test_keep_cap() {
  admin::KeepOptions k;
  k.idle_max = 2;
  Server s(kWriteDeadlineMs, admin::kOutMax, nullptr, k);
  const int a = keep_conn(s.port);
  const int b = keep_conn(s.port);
  // a used most recently: b is the least recently used idle one.
  send_line(a, "K 1 HEALTH");
  CHECK(line_of(a) == "1 OK");
  const int c = keep_conn(s.port);
  CHECK(closed_by_peer(b));
  CHECK_EQ(s.c().kept_evicted.load(), 1u);
  send_line(a, "K 2 HEALTH");
  CHECK(line_of(a) == "2 OK");
  send_line(c, "K 1 HEALTH");
  CHECK(line_of(c) == "1 OK");
  // Twice the cap in any state: refused. Hold two away (reading) so they
  // count without being idle.
  CHECK(ha::write_all(a, "K 3 HEA"));
  CHECK(ha::write_all(c, "K 2 HEA"));
  const int d = keep_conn(s.port);
  const int e = keep_conn(s.port);  // 2 reading + 2 idle: at the cap of 4
  const int f = connect_to(s.port);
  send_line(f, "KEEP 1");
  CHECK(reply_of(f) == "NO keep full");
  CHECK_EQ(s.c().keep_full.load(), 1u);
  for (int fd : {a, b, c, d, e, f}) ::close(fd);
  std::printf("K4 idle cap 2: the least recently used idle connection closed (kept_evicted=1); a handshake "
              "past 4 kept refused NO keep full\n");
}

void test_keep_health_inline() {
  std::atomic<bool> release{false};
  std::atomic<int> started{0};
  admin::KeepOptions k;
  k.idle_max = 64;
  Server s(kWriteDeadlineMs, admin::kOutMax, [&](const std::string& line) {
    started.fetch_add(1);
    while (!release.load()) ::usleep(1000);
    return "HEAVY " + line;
  }, k);
  // The idle set full.
  std::vector<int> kept;
  for (int i = 0; i < 64; ++i) kept.push_back(keep_conn(s.port));
  wait_for([&] { return s.c().kept.load() == 64; }, "64 idle kept connections");
  // The worker held by a one-shot request, kQueueMax queued behind it.
  std::vector<int> heavy;
  heavy.push_back(connect_to(s.port));
  send_line(heavy.back(), "STATUS");
  wait_for([&] { return started.load() == 1; }, "the worker holds a request");
  for (std::size_t i = 0; i < admin::kQueueMax; ++i) {
    heavy.push_back(connect_to(s.port));
    send_line(heavy.back(), "STATUS");
  }
  wait_for([&] { return s.c().queue_depth.load() == admin::kQueueMax; }, "the queue full");
  // A kept heavy request past the full queue: "<seq> NO busy", and the
  // connection stays kept.
  send_line(kept[1], "K 1 STATUS");
  CHECK(line_of(kept[1]) == "1 NO busy");
  // Kept connections active while HEALTH is measured: one with a partial
  // request line.
  CHECK(ha::write_all(kept[2], "K 1 HEA"));
  std::vector<int> half;
  for (int i = 0; i < 200; ++i) half.push_back(connect_to(s.port));
  wait_for([&] { return s.c().half.load() >= 200; }, "200 half-read connections");
  double worst = 0;
  for (int i = 0; i < 20; ++i) worst = std::max(worst, health_ms(s.port));
  CHECK_EQ(s.c().kept.load(), 64u);  // never closed to make room
  CHECK_EQ(s.c().kept_evicted.load(), 0u);
  release.store(true);
  for (int fd : heavy) {
    CHECK(reply_of(fd) == "HEAVY STATUS");
    ::close(fd);
  }
  for (int fd : half) ::close(fd);
  // The kept ones still answer.
  send_line(kept[0], "K 1 HEALTH");
  CHECK(line_of(kept[0]) == "1 OK");
  send_line(kept[1], "K 2 HEALTH");
  CHECK(line_of(kept[1]) == "2 OK");
  for (int fd : kept) ::close(fd);
  CHECK(worst < 500);
  std::printf("K5 (A6) 64 idle kept, worker stalled, queue full, 200 half-read: 20 one-shot HEALTH inline, "
              "worst %.1f ms; no kept connection closed\n", worst);
}

void test_keep_off() {
  admin::KeepOptions k;
  k.on = false;
  Server s(kWriteDeadlineMs, admin::kOutMax, nullptr, k);
  const int fd = connect_to(s.port);
  send_line(fd, "KEEP 1");
  CHECK(reply_of(fd) == "HEAVY KEEP 1");  // an unknown verb on the worker, then closed
  ::close(fd);
  CHECK_EQ(s.c().keep_ok.load(), 0u);
  std::printf("K6 keep off: KEEP 1 went to the worker as an unknown verb and the connection closed\n");
}

void test_keep_stop() {
  std::atomic<bool> release{false};
  std::atomic<int> started{0};
  auto s = std::make_unique<Server>(kWriteDeadlineMs, admin::kOutMax, [&](const std::string& line) {
    started.fetch_add(1);
    while (!release.load()) ::usleep(1000);
    return "HEAVY " + line;
  });
  const int idle = keep_conn(s->port);
  const int busy = keep_conn(s->port);
  send_line(busy, "K 1 STATUS");
  wait_for([&] { return started.load() == 1; }, "the worker holds the kept request");
  std::thread stopper([&] { s->loop->stop(); });  // waits for the running request
  CHECK(closed_by_peer(idle));  // the loop has exited: the worker's handback comes after it
  release.store(true);
  stopper.join();
  CHECK(line_of(busy) == "1 HEAVY STATUS");  // written, then handed back after stop: closed
  CHECK(closed_by_peer(busy));
  ::close(idle);
  ::close(busy);
  s.reset();
  std::printf("K7 stop with kept connections idle and with the worker: returns; both closed\n");
}

// ---- K0: ha::Conn, the client ----

// A scripted admin port: one thread per connection.
struct Fake {
  enum class Mode { NORMAL, OLD, WRONG_SEQ, EXTRA, CLOSE_IDLE, DROP_REUSED, LATE };
  u16 port = 0;
  int lfd = -1;
  std::atomic<Mode> mode{Mode::NORMAL};
  std::atomic<int> conns{0}, keeps{0}, kreqs{0}, bare{0};
  std::mutex mu;
  std::vector<std::string> lines;  // every request line received, in order
  std::vector<std::pair<int, std::string>> by_conn;  // (connection number, line)
  std::vector<int> open;           // to close at the end
  std::thread acc;
  std::atomic<bool> stop{false};

  Fake() {
    lfd = listen_any(port);
    acc = std::thread([this] {
      while (!stop.load()) {
        if (!ha::wait_fd(lfd, POLLIN, 50)) continue;
        const int fd = ::accept(lfd, nullptr, nullptr);
        if (fd < 0) continue;
        const int id = conns.fetch_add(1) + 1;
        {
          std::lock_guard<std::mutex> g(mu);
          open.push_back(fd);
        }
        std::thread([this, fd, id] { serve(fd, id); }).detach();
      }
    });
  }
  ~Fake() {
    stop.store(true);
    acc.join();
    ::close(lfd);
    std::lock_guard<std::mutex> g(mu);
    for (int fd : open) ::shutdown(fd, SHUT_RDWR);
    ::usleep(50 * 1000);  // the connection threads see it and end
  }
  void serve(int fd, int id) {
    bool kept = false;
    int answered = 0;
    while (true) {
      std::string line;
      char ch;
      while (true) {
        const ssize_t r = ::recv(fd, &ch, 1, 0);
        if (r <= 0) return;
        if (ch == '\n') break;
        line += ch;
      }
      {
        std::lock_guard<std::mutex> g(mu);
        lines.push_back(line);
        by_conn.emplace_back(id, line);
      }
      const Mode m = mode.load();
      if (!kept) {
        if (line == "KEEP 1") {
          keeps.fetch_add(1);
          if (m == Mode::OLD) {
            ha::write_all(fd, "NO unknown request\n");
            ::shutdown(fd, SHUT_RDWR);
            return;
          }
          ha::write_all(fd, "OK keep\n");
          kept = true;
          if (m == Mode::CLOSE_IDLE) {
            ::shutdown(fd, SHUT_RDWR);  // as a server's idle close
            return;
          }
          continue;
        }
        bare.fetch_add(1);
        ha::write_all(fd, "R " + line + "\n");
        ::shutdown(fd, SHUT_RDWR);
        return;
      }
      kreqs.fetch_add(1);
      const std::size_t sp = line.find(' ', 2);
      const std::string seq = line.substr(2, sp - 2), req = line.substr(sp + 1);
      if (m == Mode::DROP_REUSED && answered >= 1) {
        ::shutdown(fd, SHUT_RDWR);  // read it, close without a byte of reply
        return;
      }
      if (m == Mode::LATE && req == "SLOW") {
        ::usleep(400 * 1000);  // past the client's 200 ms deadline
      }
      if (m == Mode::WRONG_SEQ) {
        ha::write_all(fd, std::to_string(std::stoull(seq) + 1) + " R " + req + "\n");
      } else if (m == Mode::EXTRA) {
        ha::write_all(fd, seq + " R " + req + "\nX");
      } else {
        ha::write_all(fd, seq + " R " + req + "\n");
      }
      ++answered;
    }
  }
  int count(const std::string& l) {
    std::lock_guard<std::mutex> g(mu);
    int n = 0;
    for (const auto& x : lines) n += x == l ? 1 : 0;
    return n;
  }
  // The connection a line arrived on (0 if none).
  int conn_of(const std::string& l) {
    std::lock_guard<std::mutex> g(mu);
    for (const auto& [id, x] : by_conn) {
      if (x == l) return id;
    }
    return 0;
  }
  bool any_prefixed() {
    std::lock_guard<std::mutex> g(mu);
    for (const auto& x : lines) {
      if (x.rfind("K ", 0) == 0) return true;
    }
    return false;
  }
};

void wait_kept(ha::Conn& c) { wait_for([&] { return c.kept(); }, "the handshake done"); }

void test_conn_normal() {
  Fake f;
  ha::Conn c("127.0.0.1", f.port);
  std::string r;
  CHECK(c.ask("A 0", r, 1000));  // one-shot while the handshake runs
  CHECK(r == "R A 0");
  wait_kept(c);
  for (int i = 1; i <= 20; ++i) {
    CHECK(c.ask("A " + std::to_string(i), r, 1000));
    CHECK(r == "R A " + std::to_string(i));
  }
  CHECK_EQ(f.conns.load(), 2);  // the one-shot and the kept one
  CHECK_EQ(f.keeps.load(), 1);
  CHECK_EQ(f.kreqs.load(), 20);
  CHECK_EQ(f.bare.load(), 1);
  std::printf("C1 client: first request one-shot during the handshake, the next 20 on one kept connection "
              "(2 connections in all)\n");
}

void test_conn_old() {
  Fake f;
  f.mode = Fake::Mode::OLD;
  ha::Conn c("127.0.0.1", f.port);
  std::string r;
  for (int i = 0; i < 20; ++i) {
    CHECK(c.ask("A " + std::to_string(i), r, 1000));
    CHECK(r == "R A " + std::to_string(i));
    ::usleep(5000);
  }
  CHECK(!c.kept());
  CHECK_EQ(f.keeps.load(), 1);  // once per 60 s
  CHECK(!f.any_prefixed());
  std::printf("C2 client, server refusing KEEP: 20 requests one-shot, one handshake attempt, no K line sent\n");
}

void test_conn_bad_reply(Fake::Mode m, const char* what) {
  Fake f;
  ha::Conn c("127.0.0.1", f.port);
  std::string r;
  CHECK(c.ask("A 0", r, 1000));
  wait_kept(c);
  f.mode = m;
  r = "unset";
  CHECK(!c.ask("PROMOTE", r, 1000));  // not read-only: no resend; closed, never reused
  CHECK_EQ(f.count("K 1 PROMOTE"), 1);
  CHECK_EQ(f.count("PROMOTE"), 0);
  f.mode = Fake::Mode::NORMAL;
  CHECK(c.ask("B", r, 1000));  // its own reply (one-shot or a new kept connection)
  CHECK(r == "R B");
  wait_kept(c);
  CHECK(c.ask("C", r, 1000));
  CHECK(r == "R C");
  std::printf("C3 client, %s: false, closed, not resent; the next requests got their own replies\n", what);
}

void test_conn_idle_closed() {
  Fake f;
  f.mode = Fake::Mode::CLOSE_IDLE;
  ha::Conn c("127.0.0.1", f.port);
  std::string r;
  CHECK(c.ask("A", r, 1000));
  wait_kept(c);
  ::usleep(50 * 1000);  // the server's close arrives
  f.mode = Fake::Mode::NORMAL;
  CHECK(c.ask("PROMOTE", r, 1000));  // the reuse check saw the close: one-shot, sent once
  CHECK(r == "R PROMOTE");
  CHECK_EQ(f.count("PROMOTE"), 1);
  CHECK_EQ(f.kreqs.load(), 0);
  std::printf("C4 client: an idle connection the server closed is found by the reuse check; the request went "
              "one-shot, once\n");
}

void test_conn_drop_reused() {
  Fake f;
  ha::Conn c("127.0.0.1", f.port);
  std::string r;
  CHECK(c.ask("A", r, 1000));
  wait_kept(c);
  CHECK(c.ask("B", r, 1000));  // answered: the connection is now reused
  f.mode = Fake::Mode::DROP_REUSED;
  CHECK(c.ask("MOVE INFO 3", r, 1000));  // read-only: resent once, one-shot
  CHECK(r == "R MOVE INFO 3");
  CHECK_EQ(f.count("MOVE INFO 3"), 1);
  // A read-only verb with no argument ("MOVE ROLE", as the driver sends it).
  f.mode = Fake::Mode::NORMAL;
  wait_kept(c);
  CHECK(c.ask("D", r, 1000));
  f.mode = Fake::Mode::DROP_REUSED;
  CHECK(c.ask("MOVE ROLE", r, 1000));
  CHECK(r == "R MOVE ROLE");
  CHECK_EQ(f.count("MOVE ROLE"), 1);
  // Again for a verb that is not read-only: false, never resent.
  f.mode = Fake::Mode::NORMAL;
  wait_kept(c);
  CHECK(c.ask("C", r, 1000));
  f.mode = Fake::Mode::DROP_REUSED;
  CHECK(!c.ask("PROMOTE", r, 1000));
  CHECK_EQ(f.count("PROMOTE"), 0);  // only the kept copy, "K <n> PROMOTE", was sent
  std::printf("C5 client, a reused connection closed with no reply byte: MOVE INFO resent once one-shot; "
              "PROMOTE false and not resent\n");
}

void test_conn_late() {
  Fake f;
  f.mode = Fake::Mode::LATE;
  ha::Conn c("127.0.0.1", f.port);
  std::string r;
  CHECK(c.ask("A", r, 1000));
  wait_kept(c);
  CHECK(!c.ask("SLOW", r, 200));
  // At once, while the server still sleeps on SLOW: if the timed-out
  // connection were reused, its late "1 R SLOW" would be read for NEXT.
  CHECK(c.ask("NEXT", r, 1000));
  CHECK(r == "R NEXT");
  const int slow = f.conn_of("K 1 SLOW");
  CHECK(slow != 0);
  ::usleep(500 * 1000);  // the server wakes, answers SLOW into the closed connection, reads on
  int on_slow = 0;
  {
    std::lock_guard<std::mutex> g(f.mu);
    for (const auto& [id, x] : f.by_conn) on_slow += id == slow && x != "KEEP 1" && x != "K 1 SLOW" ? 1 : 0;
  }
  CHECK_EQ(on_slow, 0);  // nothing after SLOW was sent on the timed-out connection
  std::printf("C6 client: a reply late past the deadline is not returned; the next request got its own\n");
}

void test_conn_keep_off() {
  Fake f;
  ha::Conn c("127.0.0.1", f.port);
  std::string r;
  for (int i = 0; i < 5; ++i) {
    CHECK(c.ask("A", r, 1000));
    ::usleep(20 * 1000);
  }
  CHECK_EQ(f.keeps.load(), 0);
  CHECK(!f.any_prefixed());
  CHECK_EQ(f.conns.load(), 5);
  std::printf("C7 SNC_HA_KEEP=0: 5 requests, 5 one-shot connections, no KEEP sent\n");
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::pair<const char*, void (*)()>> tests = {
      {"A1", test_slow_reader}, {"A2", test_stalled_reader}, {"A3", test_bound}, {"A4", test_small},
      {"H1", test_heavy_stalled}, {"H2", test_accept_hold}, {"K1", test_keep_requests},
      {"K2", test_keep_proto}, {"K3", test_keep_deadlines}, {"K4", test_keep_cap},
      {"K5", test_keep_health_inline}, {"K6", test_keep_off}, {"K7", test_keep_stop},
      {"C1", test_conn_normal}, {"C2", test_conn_old},
      {"C3", [] { test_conn_bad_reply(Fake::Mode::WRONG_SEQ, "a wrong sequence number"); }},
      {"C3b", [] { test_conn_bad_reply(Fake::Mode::EXTRA, "bytes after the reply"); }},
      {"C4", test_conn_idle_closed}, {"C5", test_conn_drop_reused}, {"C6", test_conn_late}};
  if (!ha::keep_enabled()) {  // ctest's keep-off run (SNC_HA_KEEP=0)
    test_conn_keep_off();
    std::printf("test_admin_loop keep-off: passed\n");
    return 0;
  }
  for (const auto& [name, fn] : tests) {
    bool run = argc == 1;
    for (int k = 1; k < argc; ++k) run |= std::string(argv[k]) == name;
    if (run) fn();
  }
  std::printf("test_admin_loop: all passed\n");
  return 0;
}
