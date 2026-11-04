#!/usr/bin/env python3
"""Draws report charts from raw results. Writes SVG, needs only the standard library.

Usage:
  scripts/plot.py tradeoff results/static_final results/plots/static_tradeoff.svg
  scripts/plot.py shift results/update_final DATASET results/plots/shift.svg \
      --labels adaptive fixed alex btree

Each chart is derived only from raw.jsonl. Colours are the validated categorical
order: every index keeps its colour across charts, and each point has a tooltip.
"""
import argparse
import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]
SERIES_DARK = ["#3987e5", "#d95926", "#199e70", "#c98500"]
FONT = "-apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif"

STYLE = f"""<style>
  svg {{ font-family: {FONT}; color-scheme: light dark; }}
  .bg {{ fill: #fcfcfb; }}
  .ink {{ fill: #111511; }}
  .muted {{ fill: #5a5e5a; }}
  .grid {{ stroke: #1115111f; stroke-width: 1; }}
  .axis {{ stroke: #1115114d; stroke-width: 1; }}
  .ref {{ stroke: #5a5e5a; stroke-width: 1.5; stroke-dasharray: 5 4; }}
  {''.join(f'.s{i} {{ stroke: {c}; fill: {c}; }}' for i, c in enumerate(SERIES))}
  @media (prefers-color-scheme: dark) {{
    .bg {{ fill: #1a1a19; }}
    .ink {{ fill: #f0efec; }}
    .muted {{ fill: #c3c2b7; }}
    .grid {{ stroke: #f0efec24; }}
    .axis {{ stroke: #f0efec55; }}
    .ref {{ stroke: #c3c2b7; }}
    {''.join(f'.s{i} {{ stroke: {c}; fill: {c}; }}' for i, c in enumerate(SERIES_DARK))}
  }}
</style>"""


def load(run_dir: Path) -> list[dict]:
    with open(run_dir / "raw.jsonl") as raw:
        return [json.loads(line) for line in raw]


def nice_ticks(low: float, high: float, count: int = 5) -> list[float]:
    step = (high - low) / count
    magnitude = 10 ** math.floor(math.log10(step))
    step = min((m * magnitude for m in (1, 2, 5, 10)), key=lambda s: abs(s - step))
    first = math.ceil(low / step) * step
    ticks = []
    value = first
    while value <= high + step / 1000:
        ticks.append(round(value, 10))
        value += step
    return ticks


def text(x, y, content, cls="muted", size=12, anchor="start"):
    return f'<text x="{x:.1f}" y="{y:.1f}" class="{cls}" font-size="{size}" text-anchor="{anchor}">{content}</text>'


def legend(entries, x, y):
    parts = []
    for index, name in entries:
        parts.append(f'<rect x="{x}" y="{y - 5}" width="14" height="3" rx="1.5" class="s{index}" stroke="none"/>')
        parts.append(text(x + 20, y, name, "muted", 12))
        x += 34 + 7 * len(name)
    return parts


def tradeoff_chart(run_dir: Path, out: Path) -> None:
    """Memory against lookup latency, one panel per dataset."""
    records = load(run_dir)
    cells = defaultdict(list)
    binary = defaultdict(list)
    for record in records:
        if record["index"] == "binary":
            binary[record["dataset"]].append(record["ns_per_lookup"])
        else:
            cells[(record["dataset"], record["index"], record["epsilon"])].append(record)

    datasets = sorted(binary)
    panel_width, panel_height = 300, 280
    left, top, gap = 56, 56, 28
    width = left + len(datasets) * (panel_width + gap) + 8
    height = top + panel_height + 56
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" role="img" '
             'aria-label="Lookup time against index memory for each dataset">', STYLE,
             f'<rect class="bg" width="{width}" height="{height}"/>',
             text(left, 20, "Lookup time against index memory", "ink", 15)]
    parts += legend([(0, "This index"), (1, "PGM-index")], left, 40)
    parts.append(f'<line x1="{left + 230}" x2="{left + 254}" y1="36" y2="36" class="ref"/>')
    parts.append(text(left + 260, 40, "Sorted array, no index memory"))

    ns_values = [r["ns_per_lookup"] for rows in cells.values() for r in rows] + [v for rows in binary.values() for v in rows]
    y_ticks = nice_ticks(0, max(ns_values) * 1.08)
    y_max = y_ticks[-1]
    bytes_values = [r["index_bytes_per_key"] for rows in cells.values() for r in rows]
    x_low = math.floor(math.log10(min(bytes_values)))
    x_high = math.ceil(math.log10(max(bytes_values)))

    for position, dataset in enumerate(datasets):
        origin = left + position * (panel_width + gap)
        x_of = lambda v: origin + (math.log10(v) - x_low) / (x_high - x_low) * panel_width
        y_of = lambda v: top + panel_height - v / y_max * panel_height
        parts.append(text(origin, top - 6, dataset.split("_")[0], "ink", 13))
        for tick in y_ticks:
            parts.append(f'<line x1="{origin}" x2="{origin + panel_width}" y1="{y_of(tick):.1f}" y2="{y_of(tick):.1f}" class="grid"/>')
            if position == 0:
                parts.append(text(origin - 8, y_of(tick) + 4, f"{tick:g}", anchor="end"))
        for power in range(x_low, x_high + 1):
            label = f"{10.0 ** power:g}"
            parts.append(text(x_of(10.0 ** power), top + panel_height + 18, label, anchor="middle"))
        parts.append(f'<line x1="{origin}" x2="{origin + panel_width}" y1="{top + panel_height}" y2="{top + panel_height}" class="axis"/>')
        base = statistics.median(binary[dataset])
        parts.append(f'<line x1="{origin}" x2="{origin + panel_width}" y1="{y_of(base):.1f}" y2="{y_of(base):.1f}" class="ref"><title>sorted array {base:.0f} ns, n={len(binary[dataset])}</title></line>')
        for series, name in enumerate(("ali", "pgm")):
            points = []
            for (cell_dataset, index, epsilon), rows in sorted(cells.items(), key=lambda item: item[0][2]):
                if cell_dataset != dataset or index != name:
                    continue
                points.append((statistics.median(r["index_bytes_per_key"] for r in rows),
                               statistics.median(r["ns_per_lookup"] for r in rows), epsilon, len(rows)))
            path = " ".join(f"{x_of(x):.1f},{y_of(y):.1f}" for x, y, _, _ in points)
            parts.append(f'<polyline points="{path}" fill="none" class="s{series}" stroke-width="2" style="fill:none"/>')
            for x, y, epsilon, count in points:
                parts.append(f'<circle cx="{x_of(x):.1f}" cy="{y_of(y):.1f}" r="4" class="s{series}" stroke="none">'
                             f'<title>{name} epsilon {epsilon}: {y:.0f} ns, {x:.3f} bytes per key, n={count}</title></circle>')
    parts.append(text(left + (width - left) / 2, height - 12, "Index bytes per key, log scale. Lower left is better.", anchor="middle"))
    parts.append(f'<text transform="translate(14 {top + panel_height / 2}) rotate(-90)" class="muted" font-size="12" text-anchor="middle">ns per lookup</text>')
    parts.append("</svg>")
    out.write_text("\n".join(parts) + "\n")


def shift_chart(run_dir: Path, dataset: str, labels: list[str], out: Path) -> None:
    """Throughput through the hotspot's moves, median across replicates."""
    records = [r for r in load(run_dir) if r["workload"] == "shifting_hotspot"
               and r["mode"] == "throughput" and dataset in r["dataset"] and r["label"] in labels]
    width, height = 820, 380
    left, right, top, bottom = 56, 96, 56, 44
    inner_w, inner_h = width - left - right, height - top - bottom
    series = {}
    for label in labels:
        runs = [r for r in records if r["label"] == label]
        if not runs:
            continue
        length = min(len(r["chunk_ns"]) for r in runs)
        chunk_ops = runs[0]["chunk_ops"]
        series[label] = [statistics.median(chunk_ops / r["chunk_ns"][i] * 1e3 for r in runs) for i in range(length)]
    if not series:
        print("no shifting_hotspot records for these labels", file=sys.stderr)
        sys.exit(1)
    count = max(len(values) for values in series.values())
    y_ticks = nice_ticks(0, max(max(values) for values in series.values()) * 1.05)
    x_of = lambda i: left + i / max(count - 1, 1) * inner_w
    y_of = lambda v: top + inner_h - v / y_ticks[-1] * inner_h
    ops = records[0]["ops"]
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" role="img" '
             'aria-label="Throughput over the trace as the hotspot moves">', STYLE,
             f'<rect class="bg" width="{width}" height="{height}"/>',
             text(left, 20, "Throughput as the hotspot moves", "ink", 15)]
    parts += legend(list((i, name) for i, name in enumerate(series)), left, 40)
    for tick in y_ticks:
        parts.append(f'<line x1="{left}" x2="{left + inner_w}" y1="{y_of(tick):.1f}" y2="{y_of(tick):.1f}" class="grid"/>')
        parts.append(text(left - 8, y_of(tick) + 4, f"{tick:g}", anchor="end"))
    # The hotspot moves at every eighth of the trace, by construction.
    for phase in range(1, 8):
        x = left + phase / 8 * inner_w
        parts.append(f'<line x1="{x:.1f}" x2="{x:.1f}" y1="{top}" y2="{top + inner_h}" class="grid"/>')
    parts.append(f'<line x1="{left}" x2="{left + inner_w}" y1="{top + inner_h}" y2="{top + inner_h}" class="axis"/>')
    for index, (label, values) in enumerate(series.items()):
        path = " ".join(f"{x_of(i):.1f},{y_of(v):.1f}" for i, v in enumerate(values))
        parts.append(f'<polyline points="{path}" fill="none" class="s{index}" stroke-width="2" stroke-linejoin="round" style="fill:none"/>')
        parts.append(text(left + inner_w + 8, y_of(values[-1]) + 4, label, "muted"))
    parts.append(text(left + inner_w / 2, height - 12, f"Progress through {ops / 1e6:.0f}M operations. Vertical lines mark each hotspot move.", anchor="middle"))
    parts.append(f'<text transform="translate(14 {top + inner_h / 2}) rotate(-90)" class="muted" font-size="12" text-anchor="middle">million operations per second</text>')
    parts.append("</svg>")
    out.write_text("\n".join(parts) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("chart", choices=["tradeoff", "shift"])
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("rest", nargs="+")
    parser.add_argument("--labels", nargs="*", default=[])
    args = parser.parse_args()
    if args.chart == "tradeoff":
        out = Path(args.rest[0])
        out.parent.mkdir(parents=True, exist_ok=True)
        tradeoff_chart(args.run_dir, out)
    else:
        out = Path(args.rest[1])
        out.parent.mkdir(parents=True, exist_ok=True)
        shift_chart(args.run_dir, args.rest[0], args.labels, out)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
