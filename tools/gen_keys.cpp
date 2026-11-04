// Writes a seeded synthetic key file in the SOSD format, so that the smoke run
// and tests need no download.
//
// Shapes:
//   bursts    Event timestamps in nanoseconds: quiet gaps between tight bursts.
//   sawtooth  Runs of adjacent keys split by huge jumps. Each jump breaks the
//             line, so a piecewise model gets almost no benefit from fitting.
//
// Usage: gen_keys --shape bursts --keys 1000000 --seed 1 --out keys.bin
#include <cstdio>
#include <string>
#include <vector>

#include "ali/rng.h"
#include "ali/types.h"

namespace {

std::vector<ali::Key> bursts(std::size_t count, std::uint64_t seed) {
  ali::Rng rng(seed);
  std::vector<ali::Key> keys;
  keys.reserve(count);
  ali::Key now = 1'700'000'000ULL * 1'000'000'000ULL;  // A plausible epoch time.
  while (keys.size() < count) {
    const std::uint64_t burst = 1 + rng.below(512);
    for (std::uint64_t i = 0; i < burst && keys.size() < count; ++i) {
      now += 1 + rng.below(2'000);  // Up to 2 microseconds inside a burst.
      keys.push_back(now);
    }
    now += 1'000'000 + rng.below(1'000'000'000);  // 1 ms to 1 s between bursts.
  }
  return keys;
}

std::vector<ali::Key> sawtooth(std::size_t count, std::uint64_t seed) {
  ali::Rng rng(seed);
  std::vector<ali::Key> keys;
  keys.reserve(count);
  ali::Key key = 1;
  // The largest jump keeps the final key below UINT64_MAX at this density.
  const std::uint64_t max_jump = (~std::uint64_t{0} / (count + 1)) * 16;
  while (keys.size() < count) {
    const std::uint64_t run = 2 + rng.below(30);
    for (std::uint64_t i = 0; i < run && keys.size() < count; ++i) {
      keys.push_back(key++);
    }
    key += 1 + rng.below(max_jump);
  }
  return keys;
}

}  // namespace

int main(int argc, char** argv) {
  std::string shape = "bursts";
  std::string out;
  std::size_t count = 1'000'000;
  std::uint64_t seed = 1;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    const std::string value = argv[i + 1];
    if (flag == "--shape") shape = value;
    else if (flag == "--keys") count = std::stoull(value);
    else if (flag == "--seed") seed = std::stoull(value);
    else if (flag == "--out") out = value;
    else {
      std::fprintf(stderr, "unknown flag %s\n", flag.c_str());
      return 2;
    }
  }
  if (out.empty()) {
    std::fprintf(stderr, "--out is required\n");
    return 2;
  }
  const std::vector<ali::Key> keys =
      shape == "sawtooth" ? sawtooth(count, seed) : bursts(count, seed);
  std::FILE* file = std::fopen(out.c_str(), "wb");
  if (file == nullptr) {
    std::fprintf(stderr, "cannot write %s\n", out.c_str());
    return 1;
  }
  const std::uint64_t size = keys.size();
  std::fwrite(&size, sizeof(size), 1, file);
  std::fwrite(keys.data(), sizeof(ali::Key), keys.size(), file);
  std::fclose(file);
  std::printf("wrote %s: %zu %s keys\n", out.c_str(), keys.size(), shape.c_str());
  return 0;
}
