// A small seeded generator. std::mt19937 distributions differ across standard
// libraries, so a trace would not be reproducible across machines with them.
#pragma once

#include <cstdint>

namespace ali {

// SplitMix64: one 64-bit word of state, used here to expand a seed.
inline std::uint64_t splitmix64(std::uint64_t& state) {
  std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

// xoshiro256**, Blackman and Vigna.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) {
    for (std::uint64_t& word : state_) word = splitmix64(seed);
  }

  std::uint64_t next() {
    const std::uint64_t result = rotl(state_[1] * 5, 7) * 9;
    const std::uint64_t t = state_[1] << 17;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= t;
    state_[3] = rotl(state_[3], 45);
    return result;
  }

  // Uniform in [0, bound). Uses the high word of a 128-bit product, which is
  // close enough to unbiased for workload generation. bound must be non-zero.
  std::uint64_t below(std::uint64_t bound) {
    __extension__ using Wide = unsigned __int128;
    return static_cast<std::uint64_t>((static_cast<Wide>(next()) * bound) >> 64);
  }

  // True with probability percent / 100.
  bool chance(std::uint64_t percent) { return below(100) < percent; }

 private:
  static std::uint64_t rotl(std::uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
  }

  std::uint64_t state_[4];
};

}  // namespace ali
