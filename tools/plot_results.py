#!/usr/bin/env python3
"""Regenerate Phase 8 summary tables and confidence-interval plots from artifacts."""
from __future__ import annotations

import argparse
import csv
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests" / "bench"))
from harness import bootstrap_mean, bootstrap_ratio, load_config  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("experiment", type=Path)
    parser.add_argument("--baseline", default="tcp-cubic")
    args = parser.parse_args()
    manifest = json.loads((args.experiment / "manifest.json").read_text(encoding="utf-8"))
    config = manifest["config"]
    load_config(Path(__file__).resolve().parents[1] / "tests" / "bench" / "config.json")
    with (args.experiment / "runs.csv").open(newline="", encoding="utf-8") as stream:
        rows = [row for row in csv.DictReader(stream) if row["warmup"] == "0" and row["excluded"] == "0"]
    cells = defaultdict(list)
    for row in rows:
        cells[(row["profile"], row["workload"], row["variant"])].append(row)
    summaries = []
    for key, values in sorted(cells.items()):
        goodputs = [float(row["goodput_bps"] or 0) if row["status"] == "success" else 0.0 for row in values]
        mean, low, high = bootstrap_mean(goodputs, config["sampling"]["bootstrap_resamples"], config["final_seed"])
        summaries.append({"profile": key[0], "workload": key[1], "variant": key[2], "n": len(values), "failures": sum(row["status"] != "success" for row in values), "mean_goodput_bps": mean, "sd_goodput_bps": statistics.stdev(goodputs) if len(goodputs) > 1 else None, "ci_low_bps": low, "ci_high_bps": high})
    (args.experiment / "summary.json").write_text(json.dumps(summaries, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    ratios = []
    for profile, workload, variant in sorted(cells):
        if variant == args.baseline:
            continue
        current = {row["block_id"]: float(row["goodput_bps"] or 0) if row["status"] == "success" else 0.0 for row in cells[(profile, workload, variant)]}
        base = {row["block_id"]: float(row["goodput_bps"] or 0) if row["status"] == "success" else 0.0 for row in cells.get((profile, workload, args.baseline), [])}
        matched = [(current[block], base[block]) for block in sorted(current.keys() & base.keys())]
        point, low, high = bootstrap_ratio(matched, config["sampling"]["bootstrap_resamples"], config["final_seed"])
        ratios.append({"profile": profile, "workload": workload, "variant": variant, "baseline": args.baseline, "matched_blocks": len(matched), "ratio": point, "ci_low": low, "ci_high": high})
    (args.experiment / "ratios.json").write_text(json.dumps(ratios, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("wrote summaries; matplotlib unavailable, graph not generated", file=sys.stderr); return 0
    sustained = [row for row in summaries if row["workload"] == "sustained"]
    if sustained:
        labels = [f"{r['profile']}\n{r['variant']}" for r in sustained]
        means = [r["mean_goodput_bps"] / 1e6 for r in sustained]
        errors = [[(r["mean_goodput_bps"] - r["ci_low_bps"]) / 1e6 for r in sustained], [(r["ci_high_bps"] - r["mean_goodput_bps"]) / 1e6 for r in sustained]]
        figure, axis = plt.subplots(figsize=(max(8, len(labels) * .6), 5))
        axis.bar(labels, means, yerr=errors, capsize=3); axis.set_ylabel("Delivered goodput (Mbit/s)"); axis.tick_params(axis="x", rotation=45); figure.tight_layout()
        figure.savefig(args.experiment / "goodput.png", dpi=160); plt.close(figure)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
