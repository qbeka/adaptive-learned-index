// Types shared by every index. The behaviour they promise is in docs/API.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace ali {

using Key = std::uint64_t;
using Value = std::uint64_t;  // The fixed-size payload. See docs/API.md.

struct Entry {
  Key key;
  Value value;
  friend bool operator==(const Entry&, const Entry&) = default;
};

// Half-open key range. An absent hi means "through UINT64_MAX", which is the
// only way to include UINT64_MAX in a scan.
struct Range {
  Key lo = 0;
  std::optional<Key> hi;

  bool contains(Key key) const { return key >= lo && (!hi || key < *hi); }
};

// Returns the position of the first key not less than `key`, in [0, n].
// The loop has no data-dependent branch: the comparison feeds an addition, so
// a random query cannot cause a branch misprediction per step.
inline std::size_t branchless_lower_bound(const Key* keys, std::size_t n,
                                          Key key) {
  std::size_t lo = 0;
  std::size_t len = n;
  // Invariant: the answer is in [lo, lo + len].
  while (len > 1) {
    const std::size_t half = len / 2;
    lo += static_cast<std::size_t>(keys[lo + half - 1] < key) * half;
    len -= half;
  }
  return lo + static_cast<std::size_t>(len == 1 && keys[lo] < key);
}

// Splits sorted entries into parallel arrays and applies the duplicate rule:
// the last of a run of equal keys wins. Throws if the keys are out of order.
inline void split_sorted_entries(std::span<const Entry> entries,
                                 std::vector<Key>& keys,
                                 std::vector<Value>& values) {
  keys.clear();
  values.clear();
  keys.reserve(entries.size());
  values.reserve(entries.size());
  for (const Entry& entry : entries) {
    if (keys.empty() || entry.key > keys.back()) {
      keys.push_back(entry.key);
      values.push_back(entry.value);
    } else if (entry.key == keys.back()) {
      values.back() = entry.value;
    } else {
      throw std::invalid_argument("bulk_load: keys are not sorted");
    }
  }
}

}  // namespace ali
