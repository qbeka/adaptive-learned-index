#!/usr/bin/env python3
"""Runs one experiment spec and stores raw results with a manifest.

Usage: scripts/run_experiment.py experiments/NAME.json [--max-load N]

Each run directory holds:
  manifest.json  commit, flags, input hashes, hardware, seeds, command, elapsed
  raw.jsonl      one measured process per line, never edited afterwards

The script is resumable: it skips cells already in raw.jsonl, so rerunning the
same command after an interruption continues where it stopped. Every process is
separate, and the order within a replicate is shuffled with a fixed seed, so a
drifting machine cannot favour one index.

Before each process the script waits for the 1-minute load average to fall below
max_load. The load before and after each run is stored with it, so noisy runs
can be seen and excluded by a stated rule instead of by eye.
"""
import argparse
import hashlib
import itertools
import json
import os
import platform
import random
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
POLL_SECONDS = 30


def sh(*args: str) -> str:
    return subprocess.run(args, capture_output=True, text=True, check=True, cwd=ROOT).stdout.strip()


def md5_file(path: Path) -> str:
    digest = hashlib.md5()
    with open(path, "rb") as source:
        while chunk := source.read(1 << 24):
            digest.update(chunk)
    return digest.hexdigest()


def hardware() -> dict:
    fields = {
        "cpu": "machdep.cpu.brand_string",
        "logical_cpus": "hw.ncpu",
        "perf_core_count": "hw.perflevel0.physicalcpu",
        "memory_bytes": "hw.memsize",
        "l1d_bytes": "hw.perflevel0.l1dcachesize",
        "l2_bytes": "hw.perflevel0.l2cachesize",
        "cache_line_bytes": "hw.cachelinesize",
    }
    info = {name: sh("sysctl", "-n", key) for name, key in fields.items()}
    info["os"] = platform.platform()
    info["compiler"] = sh("clang++", "--version").splitlines()[0]
    return info


def expand_cells(spec: dict) -> list[dict]:
    """Returns every combination of the spec's matrix and indexes."""
    matrix = spec.get("matrix", {})
    names = list(matrix)
    cells = []
    for values in itertools.product(*(matrix[name] for name in names)):
        for index in spec["indexes"]:
            cell = dict(zip(names, values))
            cell["label"] = index["label"]
            cell["args"] = index["args"]
            cells.append(cell)
    return cells


def cell_id(cell: dict, replicate: int) -> str:
    fields = {k: v for k, v in cell.items() if k != "args"}
    return json.dumps({"replicate": replicate, **fields}, sort_keys=True)


def wait_for_quiet(max_load: float) -> float:
    announced = False
    while True:
        load = os.getloadavg()[0]
        if load < max_load:
            return load
        if not announced:
            print(f"waiting for load {load:.1f} to fall below {max_load}", flush=True)
            announced = True
        time.sleep(POLL_SECONDS)


def command_for(spec: dict, cell: dict, replicate: int) -> list[str]:
    command = [str(ROOT / "build" / "release" / spec["binary"])]
    for name, value in cell.items():
        if name in ("label", "args"):
            continue
        command += [f"--{name.replace('_', '-')}", str(value)]
    command += cell["args"]
    seed_flag = spec.get("seed_flag", "--seed")
    seed = spec["seeds"][replicate]
    return command + [seed_flag, str(seed)]


def write_manifest(run_dir: Path, spec: dict, spec_path: Path, max_load: float) -> None:
    path = run_dir / "manifest.json"
    if path.exists():
        return
    inputs = {}
    for dataset in spec.get("matrix", {}).get("dataset", []):
        inputs[dataset] = md5_file(ROOT / dataset)
    manifest = {
        "experiment": spec["name"],
        "spec": str(spec_path.relative_to(ROOT)),
        "spec_sha1": hashlib.sha1(spec_path.read_bytes()).hexdigest(),
        "commit": sh("git", "rev-parse", "HEAD"),
        "dirty": bool(sh("git", "status", "--porcelain", "--", "include", "bench", "Makefile")),
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "command": " ".join(sys.argv),
        "compiler_flags": "-std=c++20 -O3 -DNDEBUG -mcpu=native",
        "baselines": sh("cat", "third_party/PINS"),
        "input_md5": inputs,
        "seeds": spec["seeds"],
        "max_load": max_load,
        "hardware": hardware(),
        "timing": "steady_clock, 24 MHz tick on Apple silicon (41.7 ns resolution)",
        "thread_placement": "QOS_CLASS_USER_INTERACTIVE; macOS has no public affinity API",
    }
    path.write_text(json.dumps(manifest, indent=2) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("spec", type=Path)
    parser.add_argument("--max-load", type=float, default=None)
    args = parser.parse_args()

    spec_path = args.spec.resolve()
    spec = json.loads(spec_path.read_text())
    max_load = args.max_load if args.max_load is not None else spec.get("max_load", 8.0)
    subprocess.run(["make", "bench", "tools"], cwd=ROOT, check=True, capture_output=True)

    run_dir = ROOT / "results" / spec["name"]
    run_dir.mkdir(parents=True, exist_ok=True)
    write_manifest(run_dir, spec, spec_path, max_load)

    raw_path = run_dir / "raw.jsonl"
    done = set()
    if raw_path.exists():
        with open(raw_path) as raw:
            done = {json.loads(line)["cell_id"] for line in raw}

    cells = expand_cells(spec)
    total = len(cells) * len(spec["seeds"])
    start = time.time()
    finished = len(done)
    with open(raw_path, "a") as raw:
        for replicate in range(len(spec["seeds"])):
            order = list(cells)
            random.Random(spec["seeds"][replicate]).shuffle(order)
            for cell in order:
                identifier = cell_id(cell, replicate)
                if identifier in done:
                    continue
                load_before = wait_for_quiet(max_load)
                started = time.time()
                command = command_for(spec, cell, replicate)
                completed = subprocess.run(command, capture_output=True, text=True, cwd=ROOT)
                if completed.returncode != 0:
                    print(completed.stderr, file=sys.stderr)
                    print(f"FAILED: {' '.join(command)}", file=sys.stderr)
                    return 1
                record = json.loads(completed.stdout)
                record.update(
                    cell_id=identifier,
                    label=cell["label"],
                    replicate=replicate,
                    load_before=round(load_before, 2),
                    load_after=round(os.getloadavg()[0], 2),
                    elapsed_s=round(time.time() - started, 2),
                )
                raw.write(json.dumps(record) + "\n")
                raw.flush()
                finished += 1
                if finished % 10 == 0 or finished == total:
                    print(f"{finished}/{total} done, {time.time() - start:.0f}s", flush=True)

    manifest_path = run_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    manifest["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    manifest["records"] = finished
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"finished {spec['name']}: {finished} records in {run_dir.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
