#!/usr/bin/env python3
"""Measures whether the tests can fail: applies one deliberate bug at a time.

Usage: scripts/mutation_test.py [--out results/quality/mutation.json] [--jobs N]

Each mutant is one small source edit in a temporary copy. A mutant is killed if
the correctness suite fails on it, or hangs past the timeout. A survivor means a
bug the tests cannot see, so the score is only as honest as the list, which covers
every rule docs/API.md and the proofs in segment.h depend on. Mutants marked
equivalent cannot change behaviour, and are excluded from the score with a reason.
"""
import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TIMEOUT_SECONDS = 900
SEGMENT = "include/ali/segment.h"
DYNAMIC = "include/ali/dynamic_index.h"
POLICY = "include/ali/policy.h"
STATIC = "include/ali/static_index.h"
TYPES = "include/ali/types.h"

# (name, file, original text, replacement, reason if equivalent)
MUTANTS = [
    ("window hi loses the absent-key slot", SEGMENT, "predicted + model.under + 1, n", "predicted + model.under, n", None),
    ("window lo ignores over", SEGMENT, "predicted > model.over ? predicted - model.over : 0", "predicted", None),
    ("prediction not clamped to n", SEGMENT, "std::min<std::uint64_t>(predict(model, key), n)", "predict(model, key)", None),
    ("bracket check removed", SEGMENT, "if (bracketed) [[likely]] return pos;", "return pos;", None),
    ("bracket check ignores left neighbour", SEGMENT, "(pos == 0 || keys[pos - 1] < key) && (pos == n || keys[pos] >= key)", "(pos == n || keys[pos] >= key)", None),
    ("over measured with wrong sign", SEGMENT, "if (predicted > i) over = std::max(over, predicted - i);", "if (predicted < i) over = std::max(over, i - predicted);", None),
    ("under never measured", SEGMENT, "if (predicted < i) under = std::max(under, i - predicted);", "", None),
    ("cone upper bound not tightened", SEGMENT, "hi_num = ceil_num;", "hi_num = hi_num;", None),
    ("cone lower bound not tightened", SEGMENT, "lo_num = floor_num;", "lo_num = lo_num;", None),
    ("cone never ends a segment", SEGMENT, "if (static_cast<U128>(ceil_num) * lo_den < static_cast<U128>(lo_num) * offset) break;", "", None),
    ("epsilon no longer clamped", SEGMENT, "std::min(requested_epsilon, kMaxEpsilon)", "requested_epsilon", None),
    ("slope uses the lower bound", SEGMENT, "lo + (hi - lo) / 2", "lo", None),
    ("widen starts one too far right", SEGMENT, "lo = start + 1;\n    while", "lo = start + 2;\n    while", None),
    ("search keeps wrong half", TYPES, "keys[lo + half - 1] < key", "keys[lo + half - 1] <= key", None),
    ("duplicates keep the first value", TYPES, "values.back() = entry.value;", "", None),
    ("unsorted input accepted", TYPES, 'throw std::invalid_argument("bulk_load: keys are not sorted");', "keys.push_back(entry.key); values.push_back(entry.value);", None),
    ("range upper bound inclusive", TYPES, "key < *hi", "key <= *hi", None),
    ("static route takes the wrong segment", STATIC, "return after > 0 ? after - 1 : 0;", "return after < first_keys_.size() ? after : after - 1;", None),
    ("static bulk load clears before validating", STATIC, "    std::vector<Key> keys;\n    std::vector<Value> values;\n    split_sorted_entries", "    keys_.clear();\n    values_.clear();\n    std::vector<Key> keys;\n    std::vector<Value> values;\n    split_sorted_entries", None),
    ("dynamic bulk load clears before validating", DYNAMIC, "    split_sorted_entries(entries, keys, values);  // Throws before any change.", "    leaves_.clear();\n    first_keys_.clear();\n    size_ = 0;\n    reset_to_one_empty_leaf();\n    split_sorted_entries(entries, keys, values);", None),
    ("lookup skips the delta", DYNAMIC, "if (at < leaf.delta.size() && leaf.delta[at].key == key) {\n      result = leaf.delta[at].value;\n    }", "", None),
    ("lower_bound ignores the delta", DYNAMIC, "if (has_delta) return leaf.delta[d];", "", None),
    ("lower_bound prefers delta over a smaller base key", DYNAMIC, "if (has_base && (!has_delta || leaf.keys[b] < leaf.delta[d].key)) {", "if (has_base && !has_delta) {", None),
    ("lower_bound stops at the leaf edge", DYNAMIC, "if (!result && index + 1 < leaves_.size()) {", "if (false) {", None),
    ("write to a base key is buffered", DYNAMIC, "leaf.values[pos] = value;  // The invariant: never buffer a base key.\n      return false;", "leaf.delta.insert(leaf.delta.begin(), Entry{key, value});\n      return false;", None),
    ("write to a delta key adds a duplicate", DYNAMIC, "leaf.delta[at].value = value;\n      return false;", "leaf.delta.insert(leaf.delta.begin() + static_cast<std::ptrdiff_t>(at), Entry{key, value});\n      return false;", None),
    ("delta insert does not count towards size", DYNAMIC, "    ++size_;\n    policy_.on_write(leaf.state);", "    policy_.on_write(leaf.state);", None),
    ("rebuild loses delta values", DYNAMIC, "values.push_back(from_base ? old_leaf.values[b] : old_leaf.delta[d].value);", "values.push_back(from_base ? old_leaf.values[b] : 0);", None),
    ("rebuild merge drops the last delta key", DYNAMIC, "    while (b < old_leaf.keys.size() || d < old_leaf.delta.size()) {\n      const bool from_base =\n          d == old_leaf.delta.size() ||", "    while (b < old_leaf.keys.size() || d + 1 < old_leaf.delta.size()) {\n      const bool from_base =\n          d == old_leaf.delta.size() ||", None),
    ("split leaves stale routing keys", DYNAMIC, "first_keys_[index] = fresh_first_keys.front();", "", None),
    ("scan ignores the delta", DYNAMIC, "const Entry entry = from_base ? Entry{leaf.keys[b], leaf.values[b]}\n                                      : leaf.delta[d];", "if (!from_base) { ++d; continue; }\n        const Entry entry = Entry{leaf.keys[b], leaf.values[b]};", None),
    ("scan visits an empty range", DYNAMIC, "if (range.hi && range.lo >= *range.hi) return 0;\n", "", "Range::contains already rejects every key when lo >= hi, so the early return is only a shortcut."),
    ("rank ignores the delta", DYNAMIC, "return total + base_position(leaf, key) + delta_position(leaf, key);", "return total + base_position(leaf, key);", None),
    ("rank ignores earlier deltas", DYNAMIC, "total += leaves_[i]->keys.size() + leaves_[i]->delta.size();", "total += leaves_[i]->keys.size();", None),
    ("seal keeps its buffer capacity", DYNAMIC, "std::vector<Entry>().swap(leaf.delta);", "leaf.delta.clear();", None),
    ("seal applies when delta overlaps the base", DYNAMIC, "old_leaf.delta.front().key > old_leaf.keys.back()", "old_leaf.delta.back().key > old_leaf.keys.back()", None),
    ("delta capacity never forces a rebuild", DYNAMIC, "const bool forced = leaf.delta.size() >= config_.delta_capacity;", "const bool forced = false;", None),
    ("counters never decay", POLICY, "state.reads >>= halvings;\n    state.writes >>= halvings;", "", None),
    ("half-life not clamped", POLICY, "std::min(config.half_life_log2, 62U)", "config.half_life_log2", None),
    ("rebuild ignores the margin", POLICY, "> (1 + config_.margin) * merge_cost_ns(leaf)", "> 0 * merge_cost_ns(leaf)", None),
    ("minimum samples ignored", POLICY, "if (state.reads + state.writes < config_.min_samples) return false;", "", None),
    ("minimum delta ignored", POLICY, "if (leaf.delta_keys < config_.min_delta_keys) return false;", "", None),
    ("refill not modelled", POLICY, "const double refill = writes / 2;", "const double refill = 0;", None),
    ("reads earn nothing", POLICY, "    ++state.reads;\n", "", None),
    ("calibration can be optimised away", POLICY, 'asm volatile("" : : "r"(words.data()) : "memory");', "", None),
]


def run_suite(work: Path) -> tuple[str, str]:
    """Returns (outcome, detail). Outcome is killed, survived or invalid."""
    flags = ["clang++", "-std=c++20", "-O2", "-Iinclude", "-Itests", "-w"]
    for test in ("test_dynamic", "test_static", "test_reference"):
        built = subprocess.run(flags + [f"tests/{test}.cpp", "-o", test], cwd=work, capture_output=True, text=True)
        if built.returncode != 0:
            return "invalid", built.stderr[:300]
        try:
            ran = subprocess.run([f"./{test}"], cwd=work, capture_output=True, text=True, timeout=TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            return "killed", f"{test} timed out"
        if ran.returncode != 0:
            return "killed", f"{test}: {(ran.stderr or 'crashed').strip()[:160]}"
    return "survived", ""


def run_in_sandbox(relative: str | None, original: str | None, replacement: str | None) -> tuple[str, str]:
    """Runs the suite in a temporary copy, with one edit applied if one is given."""
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        shutil.copytree(ROOT / "include", work / "include")
        shutil.copytree(ROOT / "tests", work / "tests")
        (work / "build").mkdir()
        if relative is not None:
            target = work / relative
            target.write_text(target.read_text().replace(original, replacement))
        return run_suite(work)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=ROOT / "results" / "quality" / "mutation.json")
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args()

    for name, relative, original, replacement, equivalent in MUTANTS:
        source = (ROOT / relative).read_text()
        if source.count(original) != 1:
            print(f"STALE MUTANT: {name}: text found {source.count(original)} times", file=sys.stderr)
            return 2

    # The unmodified code must pass here first. Otherwise a failure of the sandbox
    # itself would be counted as a killed mutant, which is what happened in an
    # earlier version of this script when the sandbox had no build/ directory.
    baseline, detail = run_in_sandbox(None, None, None)
    if baseline != "survived":
        print(f"the unmodified suite fails in the sandbox: {detail}", file=sys.stderr)
        return 2

    def run_mutant(mutant: tuple) -> dict:
        name, relative, original, replacement, equivalent = mutant
        if equivalent:
            return {"name": name, "file": relative, "outcome": "equivalent", "detail": equivalent}
        started = time.time()
        outcome, detail = run_in_sandbox(relative, original, replacement)
        print(f"{outcome:9} {name}", flush=True)
        return {"name": name, "file": relative, "outcome": outcome, "detail": detail,
                "seconds": round(time.time() - started, 1)}

    with ThreadPoolExecutor(args.jobs) as pool:
        rows = list(pool.map(run_mutant, MUTANTS))

    killed = sum(r["outcome"] == "killed" for r in rows)
    survived = [r["name"] for r in rows if r["outcome"] == "survived"]
    invalid = [r["name"] for r in rows if r["outcome"] == "invalid"]
    equivalent = sum(r["outcome"] == "equivalent" for r in rows)
    scored = killed + len(survived)
    summary = {
        "commit": subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip(),
        "mutants": len(rows), "killed": killed, "survived": survived, "invalid": invalid,
        "equivalent": equivalent, "scored": scored,
        "score_percent": round(100 * killed / scored, 1) if scored else 0, "rows": rows,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(summary, indent=2) + "\n")
    print(f"killed {killed} of {scored} scored mutants, {equivalent} equivalent, {len(invalid)} invalid")
    return 0


if __name__ == "__main__":
    sys.exit(main())
