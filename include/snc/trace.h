// Causal trace lines for investigations (docs/TRACING.md). Off unless the
// environment has SNC_TRACE=1; then each SNC_TRACE writes one line to
// stderr with a single write():
//
//   T <CLOCK_MONOTONIC ns> <pid> <event> k=v ...
//
// CLOCK_MONOTONIC is shared by every process on one host, so the lines of
// several processes merge into one timeline (bench/lab/trace_merge.py).
// Disabled cost: one load and branch on a function-local static. This is
// the stopgap the ring-buffer design in docs/TRACING.md replaces; it is
// for control paths and rare events, not for per-op tracing.
#pragma once

#include <time.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace snc::trace {

inline bool enabled() {
  static const bool on = [] {
    const char* v = std::getenv("SNC_TRACE");
    return v != nullptr && v[0] == '1';
  }();
  return on;
}

inline uint64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

__attribute__((format(printf, 1, 2))) inline void emit(const char* fmt, ...) {
  char buf[512];
  int n = std::snprintf(buf, sizeof(buf), "T %llu %d ", static_cast<unsigned long long>(now_ns()),
                        static_cast<int>(::getpid()));
  va_list ap;
  va_start(ap, fmt);
  int m = std::vsnprintf(buf + n, sizeof(buf) - static_cast<size_t>(n) - 1, fmt, ap);
  va_end(ap);
  if (m < 0) return;
  size_t len = static_cast<size_t>(n) + static_cast<size_t>(m);
  if (len > sizeof(buf) - 2) len = sizeof(buf) - 2;
  buf[len++] = '\n';
  ssize_t w = ::write(2, buf, len);
  (void)w;
}

}  // namespace snc::trace

#define SNC_TRACE(...)                                     \
  do {                                                     \
    if (::snc::trace::enabled()) ::snc::trace::emit(__VA_ARGS__); \
  } while (0)
