#!/usr/bin/env python3
"""Create a transparent, reproducible report for a Phase 9 artifact set."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
from collections import Counter, defaultdict
from pathlib import Path


def number(value: str) -> float:
    return float(value) if value else 0.0


def digest(path: Path) -> str:
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(chunk)
    return checksum.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("experiment", type=Path)
    args = parser.parse_args()
    experiment = args.experiment.resolve()
    display_experiment = args.experiment.as_posix()
    manifest = json.loads((experiment / "manifest.json").read_text(encoding="utf-8"))
    with (experiment / "runs.csv").open(newline="", encoding="utf-8") as stream:
        all_rows = list(csv.DictReader(stream))
    rows = [row for row in all_rows if row["warmup"] == "0" and row["excluded"] == "0"]
    status = Counter(row["status"] for row in rows)
    grouped: dict[tuple[str, str, str], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        grouped[(row["profile"], row["workload"], row["variant"])].append(row)
    metric_coverage = {
        field: sum(bool(row[field]) for row in rows)
        for field in ("offered_bytes", "delivered_bytes", "wire_bytes", "retransmitted_bytes",
                      "goodput_bps", "latency_p95_ms", "recovery_ms", "sender_user_cpu_s",
                      "receiver_user_cpu_s")
    }
    cells = []
    for key, values in sorted(grouped.items()):
        successes = [row for row in values if row["status"] == "success"]
        cpu = [number(row["sender_user_cpu_s"]) + number(row["sender_system_cpu_s"]) +
               number(row["receiver_user_cpu_s"]) + number(row["receiver_system_cpu_s"])
               for row in successes]
        cells.append({
            "profile": key[0], "workload": key[1], "variant": key[2], "attempts": len(values),
            "successes": len(successes), "failures": len(values) - len(successes),
            "mean_delivered_bytes": sum(number(row["delivered_bytes"]) for row in successes) / len(successes) if successes else None,
            "mean_goodput_bps": sum(number(row["goodput_bps"]) for row in successes if row["goodput_bps"]) /
                sum(bool(row["goodput_bps"]) for row in successes) if any(row["goodput_bps"] for row in successes) else None,
            "mean_endpoint_cpu_s": sum(cpu) / len(cpu) if cpu else None,
        })
    artifact_files = ["manifest.json", "runs.csv", "records.csv.gz", "events.csv", "fairness.csv", "summary.json", "ratios.json"]
    hashes = {name: digest(experiment / name) for name in artifact_files if (experiment / name).exists()}
    metrics = {"schema_version": 1, "manifest_id": manifest["manifest_id"], "retained_attempts": len(rows),
               "warmup_attempts": len(all_rows) - len(rows), "status": dict(sorted(status.items())),
               "metric_coverage": metric_coverage, "cells": cells, "artifact_sha256": hashes}
    (experiment / "metrics.json").write_text(json.dumps(metrics, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    lines = [
        "# Phase 9 compact baseline report", "",
        "## Scope", "",
        f"Frozen source revision: `{manifest['repository']['revision']}`.",
        f"Artifact manifest: `{manifest['manifest_id']}`. The compact protocol used one warm-up and three retained blocks; this is descriptive evidence, not a high-power confirmatory study.", "",
        "## Retained outcomes", "",
        f"Retained attempts: **{len(rows)}**; warm-up attempts: **{len(all_rows) - len(rows)}**.",
        "", "| Status | Count |", "| --- | ---: |",
        *[f"| {name} | {count} |" for name, count in sorted(status.items())], "",
        "Every timeout and failure remains in `runs.csv`; none was excluded. `summary.json` supplies deterministic bootstrap confidence intervals for each available goodput cell, and `ratios.json` records only matched-block ratios.", "",
        "## Cell-level delivery and CPU", "",
        "| Profile | Workload | Variant | Success / attempts | Mean delivered bytes (successful) | Mean goodput bps (where measured) | Mean endpoint CPU s (successful) |",
        "| --- | --- | --- | ---: | ---: | ---: | ---: |",
    ]
    for cell in cells:
        fmt = lambda value: "N/A" if value is None else f"{value:.3f}"
        lines.append(f"| {cell['profile']} | {cell['workload']} | {cell['variant']} | {cell['successes']} / {cell['attempts']} | {fmt(cell['mean_delivered_bytes'])} | {fmt(cell['mean_goodput_bps'])} | {fmt(cell['mean_endpoint_cpu_s'])} |")
    lines += ["", "## Metric availability and limitations", "",
              "`offered_bytes`, `delivered_bytes`, CPU timing, sustained goodput, latency samples, recovery samples, raw records, controlled-drop events, queue counters, and retained packet captures are present where that workload produces them. Ordered latency/setup/recovery/file semantics are N/A for raw UDP by design.", "",
              f"The wire-byte and retransmitted-byte columns are populated for **{metric_coverage['wire_bytes']}** and **{metric_coverage['retransmitted_bytes']}** of {len(rows)} retained rows. The final collector captured only the forward impairment interface and did not aggregate bidirectional IP bytes or retransmitted payload bytes into the run rows. Therefore bidirectional overhead is **N/A**, not zero; this report does not claim compliance with the intended overhead measurement. The local full capture set and counter snapshots are retained for audit and a future corrected collection.", "",
              "## Reproduction and integrity", "",
              "Regenerate summaries and this report with:", "", "```sh",
              f"python3 tools/plot_results.py {display_experiment}",
              f"python3 tools/phase9_report.py {display_experiment}", "```", "",
              "SHA-256 checksums for the principal raw/derived files are in `metrics.json`. `records.csv.gz` is a lossless compressed copy of the raw record sample table. Full packet captures are intentionally kept outside ordinary Git because their uncompressed size exceeds repository-hosting limits; selected representative captures accompany the committed evidence package.", ""]
    (experiment / "REPORT.md").write_text("\n".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
