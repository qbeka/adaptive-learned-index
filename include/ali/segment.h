// One piecewise linear segment: how it is fitted and how a query is corrected.
//
// The model predicts a key's position inside its segment:
//
//   predicted = floor((key - first_key) * slope / 2^64)
//
// Why this form:
//   - A raw key never becomes a double. Only an integer offset is used, so
//     adjacent keys near UINT64_MAX stay distinct.
//   - Keys are distinct integers, so position grows by at most one per key
//     unit and the true slope is in [0, 1]. That fits an unsigned 0.64
//     fixed-point number.
//   - Rounding the slope changes it by less than 2^-64. Offsets are below 2^64,
//     so rounding moves a prediction by less than one position, on any data.
//   - The prediction is non-decreasing in the key. The bound below depends on
//     this.
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

#include "ali/types.h"

namespace ali {

__extension__ using U128 = unsigned __int128;

inline std::uint64_t multiply_high(std::uint64_t a, std::uint64_t b) {
  return static_cast<std::uint64_t>((static_cast<U128>(a) * b) >> 64);
}

struct SegmentModel {
  Key first_key = 0;
  std::uint64_t slope = 0;  // Unsigned 0.64 fixed point.
  // Observed on the stored keys, not assumed from the fitting tolerance:
  //   predicted - over <= true position <= predicted + under
  std::uint32_t over = 0;
  std::uint32_t under = 0;
};

inline std::uint64_t predict(const SegmentModel& model, Key key) {
  return multiply_high(key - model.first_key, model.slope);
}

// Converts the fraction num / den, in lowest-effort form, to 0.64 fixed point.
inline std::uint64_t fraction_to_slope(std::uint64_t num, std::uint64_t den) {
  if (num >= den) return std::numeric_limits<std::uint64_t>::max();
  return static_cast<std::uint64_t>((static_cast<U128>(num) << 64) / den);
}

// Fits the longest prefix of keys[0, n) that one line through (keys[0], 0) can
// cover within `epsilon` positions, and returns its length. This is the
// shrinking cone: the set of slopes that fit every key so far is an interval,
// each new key can only narrow it, and the segment ends when it becomes empty.
//
// Slope bounds stay exact fractions and are compared by cross multiplication,
// so the fit needs no division per key and no floating point. Numerators are at
// most about 2^33 and denominators below 2^64, so products fit in 128 bits.
//
// keys must be strictly increasing and n must be at least 1.
inline std::size_t fit_prefix(const Key* keys, std::size_t n,
                              std::uint64_t epsilon, std::size_t max_keys,
                              SegmentModel& model) {
  const Key first = keys[0];
  const std::size_t limit = std::min(n, max_keys);
  std::uint64_t lo_num = 0, lo_den = 1;  // Lower slope bound, starts at 0.
  std::uint64_t hi_num = 1, hi_den = 1;  // Upper slope bound, starts at 1.

  std::size_t end = 1;
  for (; end < limit; ++end) {
    const std::uint64_t offset = keys[end] - first;
    const std::uint64_t position = end;
    const std::uint64_t floor_num = position > epsilon ? position - epsilon : 0;
    const std::uint64_t ceil_num = position + epsilon;
    // This key needs a slope in [floor_num / offset, ceil_num / offset].
    if (static_cast<U128>(ceil_num) * lo_den < static_cast<U128>(lo_num) * offset) break;
    if (static_cast<U128>(floor_num) * hi_den > static_cast<U128>(hi_num) * offset) break;
    if (static_cast<U128>(floor_num) * lo_den > static_cast<U128>(lo_num) * offset) {
      lo_num = floor_num;
      lo_den = offset;
    }
    if (static_cast<U128>(ceil_num) * hi_den < static_cast<U128>(hi_num) * offset) {
      hi_num = ceil_num;
      hi_den = offset;
    }
  }

  model.first_key = first;
  model.slope = 0;
  if (end > 1) {
    const std::uint64_t lo = fraction_to_slope(lo_num, lo_den);
    const std::uint64_t hi = fraction_to_slope(hi_num, hi_den);
    model.slope = lo + (hi - lo) / 2;  // The middle of the cone.
  }

  // Measure the real error. It is what queries rely on, so it must come from
  // the stored keys and not from epsilon.
  std::uint64_t over = 0;
  std::uint64_t under = 0;
  for (std::size_t i = 0; i < end; ++i) {
    const std::uint64_t predicted = predict(model, keys[i]);
    if (predicted > i) over = std::max(over, predicted - i);
    if (predicted < i) under = std::max(under, i - predicted);
  }
  model.over = static_cast<std::uint32_t>(over);
  model.under = static_cast<std::uint32_t>(under);
  return end;
}

// Slow path of the search: finds the position by doubling steps outward from
// `start`. It needs nothing from the model, so it is correct on any input.
inline std::size_t widen_lower_bound(const Key* keys, std::size_t n,
                                     std::size_t start, Key key) {
  std::size_t lo = 0;
  std::size_t hi = n;
  if (start < n && keys[start] < key) {
    std::size_t step = 1;
    lo = start + 1;
    while (start + step < n && keys[start + step] < key) {
      lo = start + step + 1;
      step *= 2;
    }
    hi = std::min(n, start + step);
  } else if (start > 0) {
    std::size_t step = 1;
    hi = start;
    while (step <= start && keys[start - step] >= key) {
      hi = start - step;
      step *= 2;
    }
    lo = step <= start ? start - step + 1 : 0;
  }
  return lo + branchless_lower_bound(keys + lo, hi - lo, key);
}

// Returns the position of the first key >= `key` in keys[0, n), or n.
//
// Why the window is enough. Let P be the prediction, and let a < key < b be the
// stored neighbours of an absent key, at positions p and p + 1.
//   Present key at position i: P - over <= i <= P + under, by construction.
//   Absent key: the prediction is monotone, so P(a) <= P <= P(b).
//     The answer is p + 1. Since p <= P(a) + under <= P + under,
//     the answer is at most P + under + 1.
//     Since p + 1 >= P(b) - over >= P - over, the answer is at least P - over.
// So the answer is always in [P - over, P + under + 1], clamped to [0, n].
//
// The proof is checked anyway: the result is verified against its actual
// neighbours, and a failed check widens the search instead of returning a wrong
// answer. A bad model can cost time but cannot change a result. `widened` counts
// how often that happens, which should be never.
inline std::size_t corrected_lower_bound(const SegmentModel& model,
                                         const Key* keys, std::size_t n,
                                         Key key, std::uint64_t& widened) {
  if (n == 0 || key <= model.first_key) return 0;
  const std::uint64_t predicted = std::min<std::uint64_t>(predict(model, key), n);
  const std::size_t lo = predicted > model.over ? predicted - model.over : 0;
  const std::size_t hi = std::min<std::uint64_t>(predicted + model.under + 1, n);
  const std::size_t pos = lo + branchless_lower_bound(keys + lo, hi - lo, key);
  const bool bracketed =
      (pos == 0 || keys[pos - 1] < key) && (pos == n || keys[pos] >= key);
  if (bracketed) [[likely]] return pos;
  ++widened;
  return widen_lower_bound(keys, n, pos, key);
}

}  // namespace ali
