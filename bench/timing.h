// Clock, thread placement and percentile helpers for benchmarks.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace ali::bench {

inline std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// macOS has no public thread affinity API. The closest supported request is the
// highest quality-of-service class, which prefers performance cores.
inline void prefer_performance_cores() {
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

// Mean cost of one clock read pair, used to report latency overhead.
inline double clock_overhead_ns() {
  constexpr int kReads = 1'000'000;
  std::uint64_t spent = 0;
  for (int i = 0; i < kReads; ++i) {
    const std::uint64_t t0 = now_ns();
    const std::uint64_t t1 = now_ns();
    spent += t1 - t0;
  }
  return static_cast<double>(spent) / kReads;
}

// Nearest-rank percentile. Sorts `samples` in place.
inline std::uint64_t percentile(std::vector<std::uint32_t>& samples, double p) {
  if (samples.empty()) return 0;
  const auto rank = static_cast<std::size_t>(p / 100.0 * static_cast<double>(samples.size() - 1));
  std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(rank), samples.end());
  return samples[rank];
}

}  // namespace ali::bench
