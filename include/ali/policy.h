// Rebuild policies. A policy sees what happens to one leaf and decides when
// merging that leaf's delta buffer is worth it.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ali {

// What a policy may inspect about a leaf when it decides.
struct LeafView {
  std::size_t base_keys = 0;
  std::size_t delta_keys = 0;
};

// Rebuilds a leaf as soon as its delta buffer holds `threshold` keys. It does
// not look at reads at all, which makes it the reproducible baseline.
class FixedPolicy {
 public:
  struct LeafState {};

  explicit FixedPolicy(std::size_t threshold = 256) : threshold_(threshold) {}

  static constexpr bool kChecksOnRead = false;
  static constexpr bool kSamplesCosts = false;

  void on_delta_read(LeafState&) {}
  void on_write(LeafState&) {}
  void on_rebuild(std::uint64_t, std::uint64_t) {}
  void reset(LeafState&) {}

  bool should_rebuild_after_write(LeafState&, const LeafView& leaf) {
    return leaf.delta_keys >= threshold_;
  }
  bool should_rebuild_after_read(LeafState&, const LeafView&) { return false; }

  std::size_t threshold() const { return threshold_; }

 private:
  std::size_t threshold_;
};

struct AdaptiveConfig {
  // Rebuild only if expected savings exceed (1 + margin) times the merge cost.
  double margin = 1.0;
  // Decayed reads plus writes a leaf must show before any decision.
  std::uint32_t min_samples = 64;
  // Below this a delta buffer is too small to be worth merging.
  std::size_t min_delta_keys = 8;
  // Counters halve every 2^half_life_log2 buffered operations, index-wide.
  unsigned half_life_log2 = 20;
  // Replace the starting costs below with measurements while running. Tests
  // turn this off, so that rebuild decisions do not depend on the clock.
  bool measure_costs = true;
  double ns_per_step = 20.0;           // One binary search step in a delta.
  double ns_per_merged_key = 10.0;     // Merging and refitting one key.
  double ns_per_shifted_entry = 0.5;   // Moving one entry in a delta insert.
};

// Rebuilds a leaf when the work its buffer will cause exceeds the cost of
// merging it. The derivation is in docs/RESEARCH.md. In short, for a leaf with
// n base keys, d delta keys, and R reads and W writes in the recent window:
//
//   Assume the next window looks like the last one. Without a rebuild the
//   buffer averages d + W/2 keys over it. With one, it averages W/2 keys.
//
//   read saving   = R * ns_per_step * (log2(1 + d + W/2) - log2(1 + W/2))
//   write saving  = W * ns_per_shifted_entry * d / 2
//   merge cost    = (n + d) * ns_per_merged_key
//
//   Rebuild when read saving + write saving > (1 + margin) * merge cost.
//
// One unusual burst cannot cause repeated rebuilds, for three reasons. A leaf
// needs min_samples operations before any decision. The margin means savings
// must clearly exceed the cost. And a rebuild clears the leaf's counters, so
// the next rebuild has to be earned from zero.
class AdaptivePolicy {
 public:
  struct LeafState {
    std::uint32_t reads = 0;   // Reads that paid for the delta buffer.
    std::uint32_t writes = 0;  // Inserts into the delta buffer.
    std::uint32_t epoch = 0;   // When the counters were last decayed.
  };

  static constexpr bool kChecksOnRead = true;
  static constexpr bool kSamplesCosts = true;

  explicit AdaptivePolicy(AdaptiveConfig config = {})
      : config_(config),
        ns_per_step_(config.ns_per_step),
        ns_per_merged_key_(config.ns_per_merged_key),
        ns_per_shifted_entry_(config.measure_costs ? calibrate_shift_cost()
                                                   : config.ns_per_shifted_entry) {}

  void on_delta_read(LeafState& state) {
    ++events_;
    age(state);
    ++state.reads;
  }

  void on_write(LeafState& state) {
    ++events_;
    age(state);
    ++state.writes;
  }

  void reset(LeafState& state) {
    state.reads = 0;
    state.writes = 0;
    state.epoch = epoch();
  }

  bool should_rebuild_after_write(LeafState& state, const LeafView& leaf) {
    return worth_rebuilding(state, leaf);
  }

  // Deciding costs a few floating-point operations, so reads check every 16th
  // time. Reads are the common case, writes already pay for a memmove.
  bool should_rebuild_after_read(LeafState& state, const LeafView& leaf) {
    return (state.reads & 15U) == 0 && worth_rebuilding(state, leaf);
  }

  // Sampling: one delta read in 256 is timed, because a clock read costs about
  // as much as the operation.
  bool should_sample() {
    return config_.measure_costs && (++sample_tick_ & 255U) == 0;
  }

  void on_read_sample(std::uint64_t ns, std::uint64_t steps) {
    if (steps > 0) {
      blend(ns_per_step_, static_cast<double>(ns) / static_cast<double>(steps));
    }
  }

  void on_rebuild(std::uint64_t merged_keys, std::uint64_t ns) {
    if (config_.measure_costs && merged_keys > 0) {
      blend(ns_per_merged_key_, static_cast<double>(ns) / static_cast<double>(merged_keys));
    }
  }

  double ns_per_step() const { return ns_per_step_; }
  double ns_per_merged_key() const { return ns_per_merged_key_; }
  double ns_per_shifted_entry() const { return ns_per_shifted_entry_; }

  // The two sides of the decision, exposed for tests and the recording.
  double expected_saving_ns(const LeafState& state, const LeafView& leaf) const {
    const double delta = static_cast<double>(leaf.delta_keys);
    const double reads = state.reads;
    const double writes = state.writes;
    const double refill = writes / 2;
    const double read_saving = reads * ns_per_step_ *
                               (std::log2(1 + delta + refill) - std::log2(1 + refill));
    const double write_saving = writes * ns_per_shifted_entry_ * delta / 2;
    return read_saving + write_saving;
  }

  double merge_cost_ns(const LeafView& leaf) const {
    return static_cast<double>(leaf.base_keys + leaf.delta_keys) * ns_per_merged_key_;
  }

 private:
  // Times memmove directly, once. Sampling real inserts does not work here: a
  // short insert is mostly fixed overhead, which reads as a per-entry cost many
  // times too high. See docs/RESEARCH.md, failed experiments.
  static double calibrate_shift_cost() {
    constexpr std::size_t kEntries = 2048;
    constexpr int kRounds = 64;
    static_assert(sizeof(std::uint64_t) * 2 == 16);
    std::array<std::uint64_t, 2 * (kEntries + 1)> words{};
    const auto start = std::chrono::steady_clock::now();
    for (int round = 0; round < kRounds; ++round) {
      std::memmove(words.data() + 2, words.data(), 2 * kEntries * sizeof(std::uint64_t));
      // Tells the compiler the array is observed, or it deletes the memmove.
      asm volatile("" : : "r"(words.data()) : "memory");
    }
    const auto spent = std::chrono::steady_clock::now() - start;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(spent).count();
    return static_cast<double>(ns) / (static_cast<double>(kRounds) * kEntries);
  }

  // Exponential moving average with weight 1/8: recent enough to follow the
  // machine, smooth enough that one slow sample does not swing a decision.
  static void blend(double& average, double sample) {
    average += (sample - average) / 8;
  }

  std::uint32_t epoch() const {
    return static_cast<std::uint32_t>(events_ >> config_.half_life_log2);
  }

  // Applies the decay a leaf missed while nobody touched it. Decaying lazily
  // means idle leaves cost nothing, and old activity stops counting as recent.
  void age(LeafState& state) {
    const std::uint32_t now = epoch();
    if (now == state.epoch) return;
    const std::uint32_t halvings = std::min<std::uint32_t>(now - state.epoch, 31);
    state.reads >>= halvings;
    state.writes >>= halvings;
    state.epoch = now;
  }

  bool worth_rebuilding(const LeafState& state, const LeafView& leaf) const {
    if (leaf.delta_keys < config_.min_delta_keys) return false;
    if (state.reads + state.writes < config_.min_samples) return false;
    return expected_saving_ns(state, leaf) > (1 + config_.margin) * merge_cost_ns(leaf);
  }

  AdaptiveConfig config_;
  double ns_per_step_;
  double ns_per_merged_key_;
  double ns_per_shifted_entry_;
  std::uint64_t events_ = 0;
  std::uint32_t sample_tick_ = 0;
};

}  // namespace ali
