// A minimal test harness: no dependency, and a failure names file and line.
#pragma once

#include <cstdio>
#include <cstdlib>

#define CHECK(condition)                                                  \
  do {                                                                    \
    if (!(condition)) {                                                   \
      std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                   #condition);                                           \
      std::exit(1);                                                       \
    }                                                                     \
  } while (false)
