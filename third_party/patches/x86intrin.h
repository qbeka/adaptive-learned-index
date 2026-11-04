// Portable stand-ins for the only x86 intrinsics ALEX uses, so that it builds on
// arm64. They compute the same results and do not change any ALEX algorithm.
#pragma once

#include <cstdint>

inline long long _mm_popcnt_u64(std::uint64_t value) {
  return __builtin_popcountll(value);
}

inline std::uint64_t _tzcnt_u64(std::uint64_t value) {
  return value == 0 ? 64 : static_cast<std::uint64_t>(__builtin_ctzll(value));
}

inline std::uint64_t _lzcnt_u64(std::uint64_t value) {
  return value == 0 ? 64 : static_cast<std::uint64_t>(__builtin_clzll(value));
}
