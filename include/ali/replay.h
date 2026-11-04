// Replays a trace against any index and compares it with another index.
#pragma once

#include <optional>
#include <stdexcept>

#include "ali/trace.h"
#include "ali/types.h"

namespace ali {

// Everything observable about one operation, in a form two indexes can compare.
struct OpResult {
  std::uint64_t present = 0;  // Found, inserted, or the visited count for scans.
  std::uint64_t first = 0;    // Value, key, rank or scan hash.
  std::uint64_t second = 0;   // Value for lower_bound.
  friend bool operator==(const OpResult&, const OpResult&) = default;
};

// FNV-1a over 64-bit words.
inline std::uint64_t fnv_mix(std::uint64_t hash, std::uint64_t word) {
  return (hash ^ word) * 0x100000001b3ULL;
}
inline constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;

template <class Index>
OpResult apply_op(Index& index, const Op& op) {
  switch (op.kind) {
    case OpKind::kLookup: {
      const std::optional<Value> value = index.lookup(op.key);
      return value ? OpResult{1, *value, 0} : OpResult{};
    }
    case OpKind::kLowerBound: {
      const std::optional<Entry> entry = index.lower_bound(op.key);
      return entry ? OpResult{1, entry->key, entry->value} : OpResult{};
    }
    case OpKind::kInsertOrAssign:
      if constexpr (requires { index.insert_or_assign(op.key, op.arg); }) {
        return {index.insert_or_assign(op.key, op.arg) ? 1U : 0U, 0, 0};
      } else {
        throw std::logic_error("read-only index given a write");
      }
    case OpKind::kRank:
      return {1, index.rank(op.key), 0};
    case OpKind::kScan:
    case OpKind::kScanToEnd: {
      Range range{op.key, std::nullopt};
      if (op.kind == OpKind::kScan) range.hi = op.arg;
      std::uint64_t hash = kFnvOffset;
      const std::size_t visited = index.scan(range, [&hash](const Entry& entry) {
        hash = fnv_mix(fnv_mix(hash, entry.key), entry.value);
      });
      return {visited, hash, 0};
    }
  }
  return {};
}

// Folds every result into one number. Equal digests across two runs mean equal
// observable behaviour, up to a hash collision.
template <class Index>
std::uint64_t replay_digest(Index& index, const Trace& trace) {
  index.bulk_load(trace.bulk);
  std::uint64_t digest = kFnvOffset;
  for (const Op& op : trace.ops) {
    const OpResult result = apply_op(index, op);
    digest = fnv_mix(digest, result.present);
    digest = fnv_mix(digest, result.first);
    digest = fnv_mix(digest, result.second);
  }
  return fnv_mix(digest, index.size());
}

struct Divergence {
  std::size_t op_index = 0;
  Op op{};
  OpResult subject{};
  OpResult oracle{};
};

// Stops at the first operation where the two indexes disagree, then checks the
// final contents with a full scan. An op_index equal to trace.ops.size() means
// the per-operation results matched and only the final state differed.
template <class Subject, class Oracle>
std::optional<Divergence> first_divergence(Subject& subject, Oracle& oracle,
                                           const Trace& trace) {
  subject.bulk_load(trace.bulk);
  oracle.bulk_load(trace.bulk);
  for (std::size_t i = 0; i < trace.ops.size(); ++i) {
    const OpResult got = apply_op(subject, trace.ops[i]);
    const OpResult want = apply_op(oracle, trace.ops[i]);
    if (got != want) return Divergence{i, trace.ops[i], got, want};
  }
  const Op everything{OpKind::kScanToEnd, 0, 0};
  const OpResult got = apply_op(subject, everything);
  const OpResult want = apply_op(oracle, everything);
  if (got != want || subject.size() != oracle.size()) {
    return Divergence{trace.ops.size(), everything, got, want};
  }
  return std::nullopt;
}

}  // namespace ali
