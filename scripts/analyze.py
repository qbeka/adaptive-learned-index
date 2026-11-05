#!/usr/bin/env python3
"""Summarises a run directory without touching its raw data.

Usage: scripts/analyze.py results/NAME [--reference LABEL]

Every aggregate states its unit of replication: one replicate is one seed, which
is one fresh process per cell. Millions of operations inside a process are not
independent samples and are never treated as such.

Pairing: within a replicate each index replays the identical trace, so the
comparison is the ratio of throughputs within that replicate. The interval is a
percentile bootstrap over replicates with a fixed seed.
"""
import argparse
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

BOOTSTRAP_DRAWS = 10_000
RANDOM_SEED = 20261104


def load(run_dir: Path) -> list[dict]:
    with open(run_dir / "raw.jsonl") as raw:
        return [json.loads(line) for line in raw]


def check_checksums(records: list[dict]) -> int:
    """All indexes must agree on every trace. A mismatch invalidates the run."""
    seen = {}
    mismatches = 0
    for record in records:
        key = (record["dataset"], record.get("stride"), record.get("workload"),
               record.get("seed", record.get("replicate")), record.get("trace_digest"))
        if seen.setdefault(key, record["checksum"]) != record["checksum"]:
            mismatches += 1
            print(f"CHECKSUM MISMATCH {key} {record['label']}", file=sys.stderr)
    return mismatches


def bootstrap_ratio(ratios: list[float]) -> tuple[float, float]:
    """95 percent interval for the geometric mean ratio."""
    logs = np.log(np.array(ratios))
    rng = np.random.default_rng(RANDOM_SEED)
    draws = rng.choice(logs, size=(BOOTSTRAP_DRAWS, len(logs)), replace=True).mean(axis=1)
    low, high = np.percentile(draws, [2.5, 97.5])
    return float(np.exp(low)), float(np.exp(high))


def geomean(values: list[float]) -> float:
    return float(np.exp(np.mean(np.log(values))))


def summarise(records: list[dict], reference: str | None) -> list[dict]:
    throughput = [r for r in records if r["mode"] == "throughput"]
    by_cell = defaultdict(dict)
    for record in throughput:
        by_cell[(record["dataset"], record["stride"], record["workload"], record["label"])][record["replicate"]] = record

    rows = []
    for (dataset, stride, workload, label), runs in sorted(by_cell.items()):
        rates = [r["ops_per_s"] for r in runs.values()]
        row = {
            "dataset": dataset, "stride": stride, "workload": workload, "label": label,
            "n_replicates": len(rates),
            "median_mops": statistics.median(rates) / 1e6,
            "min_mops": min(rates) / 1e6, "max_mops": max(rates) / 1e6,
            "median_load": statistics.median(r["load_before"] for r in runs.values()),
        }
        for field in ("keys_merged", "bytes_moved", "rebuilds", "rebuild_ns", "worst_rebuild_ns", "leaves"):
            values = [r[field] for r in runs.values() if field in r]
            if values:
                row[field] = statistics.median(values)
        if reference and label != reference:
            base = by_cell.get((dataset, stride, workload, reference), {})
            pairs = [runs[i]["ops_per_s"] / base[i]["ops_per_s"] for i in runs if i in base]
            if pairs:
                row["ratio_to_reference"] = geomean(pairs)
                row["ratio_ci95"] = bootstrap_ratio(pairs)
                row["n_pairs"] = len(pairs)
        rows.append(row)
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--reference", default=None)
    args = parser.parse_args()

    records = load(args.run_dir)
    mismatches = check_checksums(records)
    rows = summarise(records, args.reference)
    (args.run_dir / "summary.json").write_text(json.dumps(
        {"reference": args.reference, "checksum_mismatches": mismatches, "rows": rows}, indent=2) + "\n")
    print(f"{len(records)} records, {mismatches} checksum mismatches")
    for row in rows:
        ratio = ""
        if "ratio_to_reference" in row:
            low, high = row["ratio_ci95"]
            ratio = f"  ratio {row['ratio_to_reference']:.3f} [{low:.3f}, {high:.3f}] n={row['n_pairs']}"
        print(f"{row['dataset'][:12]:12} s{row['stride']:<5} {row['workload'][:16]:16} {row['label']:24} "
              f"{row['median_mops']:7.2f} Mops/s n={row['n_replicates']}{ratio}")
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
