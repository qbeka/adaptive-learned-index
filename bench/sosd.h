// Reads SOSD datasets and reproduces SOSD's equality lookup workload.
//
// The upstream harness needs x86 and does not build on this machine, so this
// file reimplements its lookup procedure from generate.cc, commit f52f4cb:
// the same xorshift generator seeded with 42, the same draw from unique keys,
// and the same rule of skipping keys with more than 100 duplicates.
//
// SOSD stores duplicates and checks the sum of their payloads. This repository
// stores a map, so a run of equal keys becomes one entry whose value is the sum
// of the run's positions. An exact lookup then returns SOSD's expected checksum,
// with payload i at position i.
#pragma once

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "ali/types.h"

namespace ali::bench {

// Keeps every `stride`-th key. Striding preserves the distribution, unlike
// taking a prefix.
inline std::vector<Key> load_sosd_keys(const std::string& path,
                                       std::size_t stride = 1) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) throw std::runtime_error("cannot read " + path);
  std::uint64_t count = 0;
  bool ok = std::fread(&count, sizeof(count), 1, file) == 1;
  std::vector<Key> keys(count);
  ok = ok && std::fread(keys.data(), sizeof(Key), count, file) == count;
  std::fclose(file);
  if (!ok) throw std::runtime_error("bad dataset " + path);
  if (stride > 1) {
    std::size_t out = 0;
    for (std::size_t i = 0; i < keys.size(); i += stride) keys[out++] = keys[i];
    keys.resize(out);
    keys.shrink_to_fit();
  }
  return keys;
}

struct Dataset {
  std::vector<Entry> entries;       // One per distinct key.
  std::vector<std::uint32_t> runs;  // Duplicate run length per entry.
  std::size_t raw_keys = 0;
  std::size_t dropped_max_keys = 0;
};

inline Dataset make_dataset(const std::vector<Key>& keys) {
  Dataset dataset;
  dataset.raw_keys = keys.size();
  dataset.entries.reserve(keys.size());
  dataset.runs.reserve(keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (i > 0 && keys[i] < keys[i - 1]) {
      throw std::runtime_error("dataset is not sorted");
    }
    // PGM and ALEX reserve UINT64_MAX, and fb contains it. Every index gets the
    // same keys, so the key is dropped for all of them and the count reported.
    if (keys[i] == ~Key{0}) {
      ++dataset.dropped_max_keys;
      continue;
    }
    if (!dataset.entries.empty() && dataset.entries.back().key == keys[i]) {
      dataset.entries.back().value += i;
      ++dataset.runs.back();
    } else {
      dataset.entries.push_back({keys[i], i});
      dataset.runs.push_back(1);
    }
  }
  return dataset;
}

// SOSD's util::FastRandom.
class SosdRandom {
 public:
  explicit SosdRandom(std::uint64_t seed) : seed_(seed) {}
  std::uint32_t next() {
    seed_ ^= (seed_ << 13);
    seed_ ^= (seed_ >> 15);
    return static_cast<std::uint32_t>(seed_ ^= (seed_ << 5));
  }

 private:
  std::uint64_t seed_;
};

inline std::vector<Entry> sosd_equality_lookups(const Dataset& dataset,
                                                std::size_t count,
                                                std::uint64_t seed = 42) {
  constexpr std::uint32_t kMaxQualifying = 100;
  constexpr std::size_t kMaxRetries = 100;
  std::vector<Entry> lookups;
  lookups.reserve(count);
  SosdRandom random(seed);
  std::size_t retries = 0;
  const auto unique = static_cast<std::uint32_t>(dataset.entries.size());
  while (lookups.size() < count) {
    const std::uint32_t offset = random.next() % unique;
    if (dataset.runs[offset] > kMaxQualifying) {
      if (++retries > kMaxRetries) {
        throw std::runtime_error("too many retries generating lookups");
      }
      continue;
    }
    lookups.push_back(dataset.entries[offset]);
    retries = 0;
  }
  return lookups;
}

}  // namespace ali::bench
