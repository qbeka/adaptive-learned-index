// Records one real story from a benchmark trace for the website demo: a query
// from the trace, the burst of inserts that followed it into the same leaf, and
// the rebuild those inserts led to.
//
// Nothing here is staged. The trace comes from the same generator, dataset and
// seed as a report run, and its digest is written into the recording so the two
// can be matched. Costs are frozen, so the same inputs always give the same
// recording.
//
// Usage: record_demo --dataset PATH --workload NAME --seed N --ops N --stride N
//                    --epsilon N --margin X --half-life N --out site/recording.js
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "ali/dynamic_index.h"
#include "sosd.h"
#include "workload.h"

using namespace ali;
using namespace ali::bench;

namespace {

using Index = DynamicIndex<AdaptivePolicy>;

constexpr std::size_t kMinBurst = 8;
constexpr std::size_t kMaxBurst = 64;
constexpr std::size_t kMaxBaseKeys = 4096;
constexpr std::size_t kSampledPoints = 240;
constexpr std::size_t kWindowKeysShown = 24;

struct Options {
  std::string dataset;
  std::string workload = "shifting_hotspot";
  std::string out = "site/recording.js";
  std::uint64_t seed = 101;
  std::size_t ops = 20'000'000;
  std::size_t stride = 16;
  std::uint64_t epsilon = 16;
  double margin = 1.0;
  unsigned half_life = 20;
};

void apply(Index& index, const Op& op) {
  if (op.kind == OpKind::kLookup) {
    index.lookup(op.key);
  } else if (op.kind == OpKind::kInsertOrAssign) {
    index.insert_or_assign(op.key, op.arg);
  } else if (op.kind == OpKind::kScan) {
    index.scan({op.key, op.arg}, [](const Entry&) {});
  }
}

Index make_index(const Options& options) {
  DynamicConfig config;
  config.epsilon = options.epsilon;
  config.seal_tail_appends = true;
  AdaptiveConfig adaptive;
  adaptive.margin = options.margin;
  adaptive.half_life_log2 = options.half_life;
  adaptive.measure_costs = false;  // Decisions must not depend on the clock.
  return Index(config, AdaptivePolicy(adaptive));
}

// Finds the first merge that makes a readable story: a small buffer in a leaf
// small enough to draw.
std::size_t find_rebuild(const Options& options, const Trace& trace) {
  Index index = make_index(options);
  index.bulk_load(trace.bulk);
  std::uint64_t seen = 0;
  for (std::size_t i = 0; i < trace.ops.size(); ++i) {
    apply(index, trace.ops[i]);
    if (index.stats().rebuilds == seen) continue;
    seen = index.stats().rebuilds;
    const RebuildEvent& event = index.last_rebuild();
    if (!event.sealed && event.delta_keys >= kMinBurst &&
        event.delta_keys <= kMaxBurst && event.base_keys <= kMaxBaseKeys) {
      return i;
    }
  }
  return trace.ops.size();
}

void print_key_list(std::FILE* out, const std::vector<Key>& keys) {
  std::fputc('[', out);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    std::fprintf(out, "%s\"%" PRIu64 "\"", i == 0 ? "" : ",", keys[i]);
  }
  std::fputc(']', out);
}

void print_lookup(std::FILE* out, const LookupTrace& trace) {
  std::fprintf(out,
               "{\"leaf\":%zu,\"base_keys\":%zu,\"delta_keys\":%zu,\"predicted\":%" PRIu64
               ",\"window_lo\":%" PRIu64 ",\"window_hi\":%" PRIu64
               ",\"position\":%zu,\"in_base\":%s,\"in_delta\":%s}",
               trace.leaf, trace.base_keys, trace.delta_keys, trace.predicted,
               trace.window_lo, trace.window_hi, trace.base_position,
               trace.in_base ? "true" : "false", trace.in_delta ? "true" : "false");
}

// Evenly spaced stored keys, so the page can draw the leaf's real curve.
void print_leaf(std::FILE* out, Index& index, std::size_t leaf) {
  const auto keys = index.leaf_keys(leaf);
  const SegmentModel& model = index.leaf_model(leaf);
  std::fprintf(out, "{\"first_key\":\"%" PRIu64 "\",\"last_key\":\"%" PRIu64
                    "\",\"keys\":%zu,\"slope\":\"%" PRIu64 "\",\"over\":%u,\"under\":%u,\"points\":[",
               model.first_key, keys.empty() ? 0 : keys.back(), keys.size(),
               model.slope, model.over, model.under);
  const std::size_t step = std::max<std::size_t>(keys.size() / kSampledPoints, 1);
  bool first = true;
  for (std::size_t i = 0; i < keys.size(); i += step) {
    std::fprintf(out, "%s[%zu,\"%" PRIu64 "\",%" PRIu64 "]", first ? "" : ",", i, keys[i],
                 predict(model, keys[i]));
    first = false;
  }
  std::fputs("]}", out);
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    const std::string value = argv[i + 1];
    if (flag == "--dataset") options.dataset = value;
    else if (flag == "--workload") options.workload = value;
    else if (flag == "--seed") options.seed = std::stoull(value);
    else if (flag == "--ops") options.ops = std::stoull(value);
    else if (flag == "--stride") options.stride = std::stoull(value);
    else if (flag == "--epsilon") options.epsilon = std::stoull(value);
    else if (flag == "--margin") options.margin = std::stod(value);
    else if (flag == "--half-life") options.half_life = static_cast<unsigned>(std::stoul(value));
    else if (flag == "--out") options.out = value;
    else {
      std::fprintf(stderr, "unknown flag %s\n", flag.c_str());
      return 2;
    }
  }

  const Dataset dataset = make_dataset(load_sosd_keys(options.dataset, options.stride));
  std::vector<Key> universe;
  universe.reserve(dataset.entries.size());
  for (const Entry& entry : dataset.entries) universe.push_back(entry.key);
  const WorkloadTrace workload = generate_workload(
      workload_shape(options.workload), universe, options.ops, options.seed);
  const Trace& trace = workload.trace;

  const std::size_t rebuild_op = find_rebuild(options, trace);
  if (rebuild_op == trace.ops.size()) {
    std::fprintf(stderr, "no rebuild in this trace fits the demo\n");
    return 1;
  }

  // Second replay, stopping just before the rebuild, to learn the leaf and which
  // inserts filled its buffer.
  Index before = make_index(options);
  before.bulk_load(trace.bulk);
  for (std::size_t i = 0; i < rebuild_op; ++i) apply(before, trace.ops[i]);
  const Key trigger = trace.ops[rebuild_op].key;
  const std::size_t leaf = before.explain_lookup(trigger).leaf;
  std::set<Key> buffered;
  for (const Entry& entry : before.leaf_delta(leaf)) buffered.insert(entry.key);
  if (trace.ops[rebuild_op].kind == OpKind::kInsertOrAssign) buffered.insert(trigger);
  const Key leaf_lo = before.leaf_model(leaf).first_key;
  const Key leaf_hi = before.leaf_keys(leaf).back();

  // Walk back to the first insert of that buffer, and past it to a real lookup
  // that hit this leaf's base. That lookup is the query the page shows.
  std::size_t burst_start = rebuild_op;
  for (std::size_t i = rebuild_op + 1; i-- > 0;) {
    if (trace.ops[i].kind == OpKind::kInsertOrAssign && buffered.count(trace.ops[i].key) > 0) {
      burst_start = i;
    }
  }
  std::size_t query_op = trace.ops.size();
  for (std::size_t i = burst_start; i-- > 0;) {
    const Op& op = trace.ops[i];
    if (op.kind == OpKind::kLookup && op.key >= leaf_lo && op.key <= leaf_hi) {
      query_op = i;
      break;
    }
  }
  if (query_op == trace.ops.size()) {
    std::fprintf(stderr, "no lookup before the burst hit this leaf\n");
    return 1;
  }
  const Key query_key = trace.ops[query_op].key;

  // Third replay: record the story in order.
  std::FILE* out = std::fopen(options.out.c_str(), "w");
  if (out == nullptr) {
    std::fprintf(stderr, "cannot write %s\n", options.out.c_str());
    return 1;
  }
  Index index = make_index(options);
  index.bulk_load(trace.bulk);
  for (std::size_t i = 0; i < query_op; ++i) apply(index, trace.ops[i]);
  const LookupTrace query = index.explain_lookup(query_key);
  std::fprintf(out, "// Generated by bench/record_demo. Do not edit.\nwindow.RECORDING = {\n");
  std::fprintf(out,
               "\"meta\":{\"dataset\":\"%s\",\"stride\":%zu,\"workload\":\"%s\",\"seed\":%" PRIu64
               ",\"ops\":%zu,\"trace_digest\":\"%016" PRIx64 "\",\"epsilon\":%" PRIu64
               ",\"margin\":%.3f,\"half_life_log2\":%u,\"query_op\":%zu,\"burst_start_op\":%zu"
               ",\"rebuild_op\":%zu,\"ops_between\":%zu},\n",
               options.dataset.substr(options.dataset.find_last_of('/') + 1).c_str(),
               options.stride, options.workload.c_str(), options.seed,
               trace.ops.size(), workload.digest, options.epsilon, options.margin,
               options.half_life, query_op, burst_start, rebuild_op, rebuild_op - query_op);

  std::fprintf(out, "\"query_key\":\"%" PRIu64 "\",\n\"query\":", query_key);
  print_lookup(out, query);
  std::fputs(",\n\"leaf_before\":", out);
  print_leaf(out, index, query.leaf);

  // The keys the correction search walks over.
  const auto base = index.leaf_keys(query.leaf);
  const std::size_t shown_lo = query.window_lo;
  const std::size_t shown_hi = std::min<std::size_t>(query.window_hi + 1, base.size());
  std::vector<Key> shown;
  for (std::size_t i = shown_lo; i < shown_hi && shown.size() < kWindowKeysShown * 4; ++i) {
    shown.push_back(base[i]);
  }
  std::fprintf(out, ",\n\"window_first_position\":%zu,\n\"window_keys\":", shown_lo);
  print_key_list(out, shown);

  // The burst: inserts into this leaf, in trace order.
  std::fputs(",\n\"inserts\":[", out);
  bool first = true;
  std::uint64_t reads_between = 0;
  for (std::size_t i = query_op; i < rebuild_op; ++i) {
    const Op& op = trace.ops[i];
    const bool ours = op.kind == OpKind::kInsertOrAssign && buffered.count(op.key) > 0;
    apply(index, op);
    if (ours) {
      const LookupTrace after = index.explain_lookup(op.key);
      std::fprintf(out, "%s{\"op\":%zu,\"key\":\"%" PRIu64 "\",\"delta_keys\":%zu}",
                   first ? "" : ",", i, op.key, after.delta_keys);
      first = false;
    } else if (op.kind == OpKind::kLookup) {
      ++reads_between;
    }
  }
  std::fputs("],\n", out);

  // The rebuild itself.
  const std::uint64_t rebuilds_before = index.stats().rebuilds;
  const bool trigger_is_write = trace.ops[rebuild_op].kind == OpKind::kInsertOrAssign;
  apply(index, trace.ops[rebuild_op]);
  if (index.stats().rebuilds != rebuilds_before + 1) {
    std::fprintf(stderr, "expected a rebuild at op %zu\n", rebuild_op);
    return 1;
  }
  const RebuildEvent& event = index.last_rebuild();
  std::fprintf(out,
               "\"rebuild\":{\"trigger\":\"%s\",\"base_keys\":%zu,\"delta_keys\":%zu,"
               "\"new_leaves\":%zu,\"expected_saving_ns\":%.1f,\"merge_cost_ns\":%.1f,"
               "\"margin\":%.3f,\"reads_between\":%" PRIu64 "},\n",
               trigger_is_write ? "write" : "read", event.base_keys, event.delta_keys,
               event.new_leaves, event.expected_saving_ns, event.merge_cost_ns,
               options.margin, reads_between);
  const LookupTrace after = index.explain_lookup(query_key);
  // Leaf numbers shift when other leaves split, so the page gets an offset into
  // the new leaves, not an absolute number.
  const std::size_t first_new = index.explain_lookup(leaf_lo).leaf;
  std::fprintf(out, "\"query_after_leaf_offset\":%zu,\n\"query_after\":", after.leaf - first_new);
  print_lookup(out, after);
  std::fputs(",\n\"leaf_after\":[", out);
  for (std::size_t i = 0; i < event.new_leaves; ++i) {
    if (i > 0) std::fputc(',', out);
    print_leaf(out, index, first_new + i);
  }
  std::fputs("]\n};\n", out);
  std::fclose(out);
  std::printf("wrote %s: query at op %zu, %zu inserts, rebuild at op %zu into %zu leaves\n",
              options.out.c_str(), query_op, buffered.size(), rebuild_op, event.new_leaves);
  return 0;
}
