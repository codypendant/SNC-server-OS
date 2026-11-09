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
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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
  send_line(fd, "HEALTH");
  const std::string r = read_all(fd, 0, 4096);
  ::close(fd);
  CHECK(r == "OK\n");
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

struct Server {
  u16 port = 0;
  std::unique_ptr<admin::Loop> loop;
  explicit Server(int write_ms = kWriteDeadlineMs, std::size_t out_max = admin::kOutMax) {
    const int lfd = listen_any(port);
    admin::Loop::Handlers h;
    h.inline_verb = [](const std::string& line, std::string& reply) {
      if (line == "HEALTH") {
        reply = "OK";
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
    h.heavy = [](const std::string& line) { return "HEAVY " + line; };
    loop = std::make_unique<admin::Loop>(lfd, std::move(h), write_ms, out_max);
    loop->start();
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
  Server s;
  constexpr std::size_t kBig = 16u << 20;
  const int fd = connect_to(s.port, 4096);
  send_line(fd, "BIG " + std::to_string(kBig));
  wait_for([&] { return s.c().out.load() == 1; }, "the reply waiting for its reader");
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
  wait_for([&] { return s.c().out.load() == 0; }, "all closed (deadline or error)");
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

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::pair<const char*, void (*)()>> tests = {
      {"A1", test_slow_reader}, {"A2", test_stalled_reader}, {"A3", test_bound}, {"A4", test_small}};
  for (const auto& [name, fn] : tests) {
    bool run = argc == 1;
    for (int k = 1; k < argc; ++k) run |= std::string(argv[k]) == name;
    if (run) fn();
  }
  std::printf("test_admin_loop: all passed\n");
  return 0;
}
