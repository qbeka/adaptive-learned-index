#!/usr/bin/env python3
"""Applies the selection rule in experiments/PLAN.md and writes the frozen spec.

Usage: scripts/select_config.py [--write]

For each policy family it picks the one configuration with the highest geometric
mean throughput across all workloads and validation seeds. Without --write it only
prints the ranking.
"""
import argparse
import json
import math
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FAMILIES = ("fixed", "adaptive", "btree")
SEEDS = [101, 102, 103, 104, 105, 106, 107, 108]
VARIANTS = [
    {"dataset": "data/sosd/wiki_ts_200M_uint64", "stride": 1},
    {"dataset": "data/sosd/osm_cellids_200M_uint64", "stride": 4},
    {"dataset": "data/sosd/wiki_ts_200M_uint64", "stride": 2048},
]
WORKLOADS = ["read_heavy", "write_heavy", "append_only", "shifting_hotspot"]
MODES = ["throughput", "latency", "memory"]


def rank(records: list[dict]) -> dict[str, float]:
    logs = defaultdict(list)
    for record in records:
        logs[record["label"]].append(math.log(record["ops_per_s"]))
    expected = max(len(values) for values in logs.values())
    incomplete = [label for label, values in logs.items() if len(values) != expected]
    if incomplete:
        print(f"incomplete configurations: {incomplete}", file=sys.stderr)
        sys.exit(1)
    return {label: math.exp(sum(values) / len(values)) for label, values in logs.items()}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()

    validation = json.loads((ROOT / "experiments/update_validation.json").read_text())
    with open(ROOT / "results/update_validation/raw.jsonl") as raw:
        records = [json.loads(line) for line in raw]
    scores = rank(records)

    chosen = {}
    for family in FAMILIES:
        ranked = sorted((label for label in scores if label.startswith(family)),
                        key=scores.get, reverse=True)
        print(family)
        for label in ranked:
            print(f"  {label:24} {scores[label] / 1e6:7.3f} Mops/s geomean")
        chosen[family] = ranked[0]
    print(f"selected: {chosen}")
    if not args.write:
        return 0

    args_of = {index["label"]: index["args"] for index in validation["indexes"]}
    # The final labels name the role, and the arguments keep the chosen settings.
    indexes = [{"label": family, "args": args_of[label]} for family, label in chosen.items()]
    indexes.append({"label": "alex", "args": args_of["alex"]})
    spec = {
        "name": "update_final",
        "binary": "update_bench",
        "max_load": validation["max_load"],
        "seeds": SEEDS,
        "selected_from": chosen,
        "variants": VARIANTS,
        "matrix": {"ops": validation["matrix"]["ops"], "workload": WORKLOADS, "mode": MODES},
        "indexes": indexes,
    }
    path = ROOT / "experiments/update_final.json"
    path.write_text(json.dumps(spec, indent=2) + "\n")
    print(f"wrote {path.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
