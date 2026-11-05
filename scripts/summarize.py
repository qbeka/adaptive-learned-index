#!/usr/bin/env python3
"""Turns the final raw results into the report's tables and verdicts.

Usage: scripts/summarize.py

Reads:  results/{static_final,static_warm,update_final,update_latency,
        update_memory,update_work}/raw.jsonl
Writes: results/summary.md          every table the report quotes
        results/decisions.json      the H1, H2 and H3 verdicts
        results/headline.json       the figures the site shows

Nothing here is edited by hand. The decision rule is the one in
experiments/PLAN.md. One replicate is one seed. All intervals are percentile
bootstrap intervals over replicates, so with 8 seeds they cannot be narrower than
the spread of those 8 values.
"""
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze import bootstrap_ratio, check_checksums, geomean, load  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results"
WORKLOADS = ["read_heavy", "write_heavy", "append_only", "shifting_hotspot"]
BASELINES = ["fixed", "alex", "btree"]
VARIANTS = [
    ("wiki_ts_200M_uint64", 1, "primary"),
    ("osm_cellids_200M_uint64", 4, "hostile"),
    ("wiki_ts_200M_uint64", 2048, "warm"),
]


def median(values) -> float:
    return statistics.median(values)


def by_seed(records: list[dict], **fields) -> dict[int, dict]:
    """The records matching `fields`, keyed by seed."""
    return {r["seed"]: r for r in records if all(r.get(k) == v for k, v in fields.items())}


def paired(a: dict[int, dict], b: dict[int, dict], field: str) -> list[float]:
    return [a[seed][field] / b[seed][field] for seed in sorted(a) if seed in b]


def interval(ratios: list[float]) -> tuple[float, float, float]:
    low, high = bootstrap_ratio(ratios)
    return geomean(ratios), low, high


def table(headers: list[str], rows: list[list]) -> list[str]:
    lines = ["| " + " | ".join(headers) + " |", "|" + "---|" * len(headers)]
    lines += ["| " + " | ".join(str(cell) for cell in row) + " |" for row in rows]
    return lines + [""]


def fmt_ratio(result: tuple[float, float, float]) -> str:
    return f"{result[0]:.3f} [{result[1]:.3f}, {result[2]:.3f}]"


def load_stats(records: list[dict]) -> str:
    loads = [r["load_before"] for r in records]
    return f"median {median(loads):.0f}, range {min(loads):.0f} to {max(loads):.0f}"


def static_section(name: str, title: str) -> tuple[list[str], dict]:
    records = load(RESULTS / name)
    if not all(r["checksum_ok"] for r in records):
        raise SystemExit(f"{name}: a lookup checksum failed")
    if check_checksums(records):
        raise SystemExit(f"{name}: indexes disagree")
    lines = [f"## {title}", "",
             f"{len(records)} processes, seeds {sorted({r['seed'] for r in records})}, "
             f"load before each process: {load_stats(records)}.", ""]
    facts = {}
    for dataset in sorted({r["dataset"] for r in records}):
        rows = []
        subset = [r for r in records if r["dataset"] == dataset]
        binary = by_seed(subset, index="binary")
        lines.append(f"### {dataset}, {subset[0]['keys']:,} keys, "
                     f"{subset[0]['dropped_max_keys']} dropped at UINT64_MAX")
        lines.append("")
        rows.append(["sorted array", "", f"{median(r['ns_per_lookup'] for r in binary.values()):.0f}",
                     f"{median(r['p50_ns'] for r in binary.values())}",
                     f"{median(r['p99_ns'] for r in binary.values())}", "0", "", ""])
        for epsilon in sorted({r["epsilon"] for r in subset if r["index"] == "ali"}):
            mine = by_seed(subset, index="ali", epsilon=epsilon)
            pgm = by_seed(subset, index="pgm", epsilon=epsilon)
            for label, runs in (("this index", mine), ("PGM", pgm)):
                vs_binary = interval(paired(runs, binary, "ns_per_lookup"))
                vs_pgm = fmt_ratio(interval(paired(mine, pgm, "ns_per_lookup"))) if runs is mine else ""
                rows.append([label, epsilon, f"{median(r['ns_per_lookup'] for r in runs.values()):.0f}",
                             f"{median(r['p50_ns'] for r in runs.values())}",
                             f"{median(r['p99_ns'] for r in runs.values())}",
                             f"{median(r['index_bytes_per_key'] for r in runs.values()):.4f}",
                             fmt_ratio(vs_binary), vs_pgm])
                facts[(dataset, label, epsilon)] = {
                    "ns": median(r["ns_per_lookup"] for r in runs.values()),
                    "bytes_per_key": median(r["index_bytes_per_key"] for r in runs.values()),
                    "vs_binary": vs_binary,
                }
        lines += table(["Index", "Epsilon", "ns per lookup", "p50 ns", "p99 ns", "Index bytes per key",
                        "Time vs sorted array", "Time vs PGM, same epsilon"], rows)
    return lines, facts


def build_section(records: list[dict]) -> list[str]:
    rows = []
    for dataset, stride, role in VARIANTS:
        for label in ("adaptive", "alex", "btree"):
            runs = [r for r in records if r["dataset"] == dataset and r["stride"] == stride and r["label"] == label]
            if runs:
                rows.append([role, label, f"{median(r['build_ns'] for r in runs) / 1e9:.2f}",
                             f"{runs[0]['bulk_keys']:,}"])
    return ["## Construction, update indexes", ""] + table(["Data", "Index", "Build seconds", "Bulk keys"], rows)


def update_section(records: list[dict], latency: list[dict], memory: list[dict]) -> tuple[list[str], dict]:
    lines = ["## Update throughput", "",
             f"{len(records)} processes, load before each process: {load_stats(records)}.",
             "Ratios are adaptive over the named index, as a geometric mean of paired seeds "
             "with a 95 percent bootstrap interval. Above 1 means adaptive is faster.", ""]
    facts = {}
    for dataset, stride, role in VARIANTS:
        subset = [r for r in records if r["dataset"] == dataset and r["stride"] == stride]
        if not subset:
            continue
        rows = []
        for workload in WORKLOADS:
            cells = {label: by_seed(subset, workload=workload, label=label)
                     for label in ["adaptive", *BASELINES]}
            row = [workload] + [f"{median(r['ops_per_s'] for r in cells[l].values()) / 1e6:.2f}"
                                for l in ["adaptive", *BASELINES]]
            for baseline in BASELINES:
                result = interval(paired(cells["adaptive"], cells[baseline], "ops_per_s"))
                facts[(role, workload, baseline)] = result
                row.append(fmt_ratio(result))
            rows.append(row)
        lines.append(f"### {role}: {dataset}, stride {stride}, {subset[0]['bulk_keys']:,} bulk keys, "
                     f"{subset[0]['ops']:,} operations, n = {len({r['seed'] for r in subset})} seeds")
        lines.append("")
        lines += table(["Workload", "adaptive Mops/s", "fixed", "alex", "btree",
                        "vs fixed", "vs alex", "vs btree"], rows)

    lines += ["## Rebuild work", "", "Medians across seeds, from the throughput runs.", ""]
    rows = []
    for dataset, stride, role in VARIANTS:
        for workload in WORKLOADS:
            for label in ("adaptive", "fixed"):
                runs = [r for r in records if r["dataset"] == dataset and r["stride"] == stride
                        and r["workload"] == workload and r["label"] == label]
                if not runs:
                    continue
                rows.append([role, workload, label, f"{median(r['rebuilds'] for r in runs):,.0f}",
                             f"{median(r['keys_merged'] for r in runs):,.0f}",
                             f"{median(r['bytes_moved'] for r in runs) / 1e9:.2f}",
                             f"{median(r['rebuild_ns'] for r in runs) / 1e9:.2f}",
                             f"{median(r['worst_rebuild_ns'] for r in runs) / 1e3:.0f}"])
    lines += table(["Data", "Workload", "Policy", "Rebuilds", "Keys merged", "GB moved",
                    "Rebuild s", "Worst rebuild us"], rows)

    lines += ["## Latency", "",
              f"{len(latency)} processes. Clock ticks are 41.7 ns, and each value includes the clock read.", ""]
    rows = []
    for dataset, stride, role in VARIANTS:
        for workload in WORKLOADS:
            for label in ["adaptive", *BASELINES]:
                runs = [r for r in latency if r["dataset"] == dataset and r["stride"] == stride
                        and r["workload"] == workload and r["label"] == label]
                if not runs:
                    continue
                rows.append([role, workload, label, median(r["read_p50_ns"] for r in runs),
                             median(r["read_p99_ns"] for r in runs), median(r["write_p50_ns"] for r in runs),
                             median(r["write_p99_ns"] for r in runs),
                             f"{median(r['worst_op_ns'] for r in runs) / 1e3:.0f}"])
    lines += table(["Data", "Workload", "Index", "Read p50 ns", "Read p99 ns", "Write p50 ns",
                    "Write p99 ns", "Worst op us"], rows)

    lines += ["## Memory", "",
              f"{len(memory)} processes. Bytes per key at allocator capacity, keys and payloads included.", ""]
    rows = []
    for dataset, stride, role in VARIANTS:
        for workload in WORKLOADS:
            for label in ["adaptive", *BASELINES]:
                runs = [r for r in memory if r["dataset"] == dataset and r["stride"] == stride
                        and r["workload"] == workload and r["label"] == label]
                if not runs:
                    continue
                final = median(r["final_bytes_per_key"] for r in runs)
                facts[("memory", role, workload, label)] = final
                rows.append([role, workload, label, f"{median(r['loaded_bytes_per_key'] for r in runs):.2f}",
                             f"{final:.2f}", f"{median(r['peak_bytes_per_key'] for r in runs):.2f}"])
    lines += table(["Data", "Workload", "Index", "Loaded", "Final", "Peak"], rows)
    return lines, facts


def work_section(records: list[dict]) -> list[str]:
    lines = ["## Work counters", "",
             "From the instrumented binary, never timed. Delta steps and shifted entries are "
             "the two costs the policy trades against merging.", ""]
    rows = []
    for dataset, stride, role in VARIANTS:
        for workload in WORKLOADS:
            for label in ("adaptive", "fixed"):
                runs = [r for r in records if r["dataset"] == dataset and r["stride"] == stride
                        and r["workload"] == workload and r["label"] == label]
                if not runs:
                    continue
                rows.append([role, workload, label,
                             f"{median(r['delta_steps'] for r in runs):,.0f}",
                             f"{median(r['delta_shifted'] for r in runs):,.0f}",
                             f"{median(r['window_steps'] for r in runs):,.0f}"])
    return lines + table(["Data", "Workload", "Policy", "Delta search steps", "Entries shifted", "Window steps"], rows)


def decide(records: list[dict], memory: list[dict], role: str, dataset: str, stride: int) -> dict:
    """Applies the rule in experiments/PLAN.md to one data split."""
    subset = [r for r in records if r["dataset"] == dataset and r["stride"] == stride]
    adaptive = {w: by_seed(subset, workload=w, label="adaptive") for w in WORKLOADS}
    fixed = {w: by_seed(subset, workload=w, label="fixed") for w in WORKLOADS}
    per_workload = {w: interval(paired(adaptive[w], fixed[w], "ops_per_s")) for w in WORKLOADS}
    h1 = per_workload["shifting_hotspot"]

    seeds = sorted(set.intersection(*(set(adaptive[w]) & set(fixed[w]) for w in WORKLOADS)))
    per_seed = [geomean([adaptive[w][s]["ops_per_s"] / fixed[w][s]["ops_per_s"] for w in WORKLOADS]) for s in seeds]
    h2_mean = interval(per_seed)
    floor_ok = all(result[2] >= 0.95 for result in per_workload.values())

    h3 = {}
    for workload in WORKLOADS:
        a = median(r["bytes_moved"] for r in adaptive[workload].values())
        f = median(r["bytes_moved"] for r in fixed[workload].values())
        h3[workload] = {"adaptive_bytes": a, "fixed_bytes": f, "ratio": a / f if f else None}

    subset_memory = [r for r in memory if r["dataset"] == dataset and r["stride"] == stride]
    memory_ratio = {}
    for workload in WORKLOADS:
        a = by_seed(subset_memory, workload=workload, label="adaptive")
        f = by_seed(subset_memory, workload=workload, label="fixed")
        memory_ratio[workload] = geomean(paired(a, f, "final_bytes_per_key"))
    memory_ok = all(value <= 1.05 for value in memory_ratio.values())

    h1_holds = h1[1] > 1.0
    h2_holds = h2_mean[1] > 1.0 and floor_ok
    return {
        "split": role, "n_seeds": len(seeds),
        "h1": {"ratio": h1, "holds": h1_holds},
        "h2": {"geomean_ratio": h2_mean, "per_workload": per_workload, "floor_ok": floor_ok, "holds": h2_holds},
        "h3": {"per_workload": h3, "holds": all(v["ratio"] is not None and v["ratio"] < 1 for v in h3.values())},
        "memory": {"ratio_final_bytes": memory_ratio, "within_5_percent": memory_ok},
        "claim_holds": bool(h1_holds and h2_holds and memory_ok),
    }


def decision_lines(decision: dict) -> list[str]:
    h1, h2, h3 = decision["h1"], decision["h2"], decision["h3"]
    lines = [f"## Decision, {decision['split']} split, n = {decision['n_seeds']} seeds", "",
             f"- H1, adaptive over fixed on shifting_hotspot: {fmt_ratio(h1['ratio'])}. "
             f"{'Holds' if h1['holds'] else 'Does not hold'}.",
             f"- H2, geometric mean over workloads: {fmt_ratio(h2['geomean_ratio'])}, floor of 0.95 on every "
             f"workload {'met' if h2['floor_ok'] else 'not met'}. {'Holds' if h2['holds'] else 'Does not hold'}."]
    for workload, result in h2["per_workload"].items():
        lines.append(f"  - {workload}: {fmt_ratio(result)}")
    lines.append(f"- H3, fewer bytes moved on every workload: {'holds' if h3['holds'] else 'does not hold'}.")
    for workload, values in h3["per_workload"].items():
        ratio = "n/a" if values["ratio"] is None else f"{values['ratio']:.3f}"
        lines.append(f"  - {workload}: adaptive over fixed {ratio}")
    memory = decision["memory"]
    lines.append(f"- Memory within 5 percent of fixed: {'yes' if memory['within_5_percent'] else 'no'}, "
                 + ", ".join(f"{w} {v:.3f}" for w, v in memory["ratio_final_bytes"].items()) + ".")
    lines.append(f"- Claim {'holds' if decision['claim_holds'] else 'does not hold'} on this split.")
    return lines + [""]


def main() -> int:
    update = load(RESULTS / "update_final")
    latency = load(RESULTS / "update_latency")
    memory = load(RESULTS / "update_memory")
    work = load(RESULTS / "update_work")
    mismatches = sum(check_checksums(records) for records in (update, latency, memory, work))
    if mismatches:
        raise SystemExit(f"{mismatches} checksum mismatches, results are invalid")

    lines = ["# Result tables", "", "Generated by `scripts/summarize.py` from raw results. Do not edit.", ""]
    decisions = [decide(update, memory, role, dataset, stride) for dataset, stride, role in VARIANTS]
    for decision in decisions:
        lines += decision_lines(decision)
    static_lines, static_facts = static_section("static_final", "Static lookups, larger than cache")
    warm_lines, warm_facts = static_section("static_warm", "Static lookups, warm cache")
    update_lines, update_facts = update_section(update, latency, memory)
    lines += static_lines + warm_lines + update_lines + build_section(update) + work_section(work)
    (RESULTS / "summary.md").write_text("\n".join(lines) + "\n")
    (RESULTS / "decisions.json").write_text(json.dumps(decisions, indent=2) + "\n")
    print(f"wrote results/summary.md and results/decisions.json, 0 checksum mismatches across "
          f"{len(update) + len(latency) + len(memory) + len(work)} update processes")
    for decision in decisions:
        print("\n".join(decision_lines(decision)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
