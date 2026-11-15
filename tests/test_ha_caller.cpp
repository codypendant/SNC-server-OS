// ha::Caller labels for the per-request trace (docs/HEALTH_BASELINE.md).
// Run twice by ctest: with SNC_TRACE unset, every scope must do nothing
// (the path stays empty); with SNC_TRACE=1, scopes nest and unwind and
// Adopt restores the thread's own path.
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "ha.h"

using namespace snc;

static int fails = 0;
#define CHECK(c)                                              \
  do {                                                        \
    if (!(c)) {                                               \
      std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
      ++fails;                                                \
    }                                                         \
  } while (0)

int main() {
  const bool on = trace::enabled();
  {
    const ha::Caller a("watch");
    {
      const ha::Caller b(std::string("propose"));
      CHECK(ha::caller_path() == (on ? "watch/propose" : ""));
      std::string seen;
      std::thread t([&, path = ha::caller_path()] {
        const ha::Caller::Adopt from(path);
        const ha::Caller c("read_latest");
        seen = ha::caller_path();
      });
      t.join();
      CHECK(seen == (on ? "watch/propose/read_latest" : ""));
      {
        const ha::Caller::Adopt p("promote");
        CHECK(ha::caller_path() == (on ? "promote" : ""));
      }
      CHECK(ha::caller_path() == (on ? "watch/propose" : ""));
    }
    CHECK(ha::caller_path() == (on ? "watch" : ""));
  }
  CHECK(ha::caller_path().empty());
  std::printf("test_ha_caller (trace %s): %s\n", on ? "on" : "off", fails ? "FAIL" : "ok");
  return fails ? 1 : 0;
}
