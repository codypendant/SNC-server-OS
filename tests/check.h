// Minimal test assertions: no framework dependency (handoff rule 2).
#pragma once

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                             \
  do {                                                                          \
    if (!(cond)) {                                                              \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                             \
    }                                                                           \
  } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))
