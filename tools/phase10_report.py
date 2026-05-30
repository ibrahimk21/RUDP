#!/usr/bin/env python3
"""Generate the compact Phase 10 Sat comparison report from frozen artifacts."""
from __future__ import annotations

import argparse
import csv
import json
import statistics
from collections import Counter, defaultdict
from pathlib import Path


def value(row: dict[str, str], key: str) -> float | None:
    return float(row[key]) if row.get(key) not in (None, "") else None


def main() -> int:
    parser = argparse.ArgumentParser(); parser.add_argument("experiment", type=Path)
    args = parser.parse_args(); root = args.experiment
    with (root / "runs.csv").open(newline="", encoding="utf-8") as stream:
        rows = [row for row in csv.DictReader(stream) if row["warmup"] == "0" and row["excluded"] == "0"]
    profile_by_run = {row["run_id"]: row["profile"] for row in rows}
    with (root / "summary.json").open(encoding="utf-8") as stream:
        summary = {(row["profile"], row["workload"], row["variant"]): row for row in json.load(stream)}
    outcomes = Counter(row["status"] for row in rows)
    def mean(profile: str, variant: str) -> float | None:
        row = summary.get((profile, "sustained", variant)); return None if row is None else row["mean_goodput_bps"]
    terrestrial_sat, terrestrial_raw = mean("terrestrial", "rudp-sat"), mean("terrestrial", "raw-udp")
    geo_sat, geo_cubic, geo_bbr = mean("geo", "rudp-sat"), mean("geo", "tcp-cubic"), mean("geo", "tcp-bbr")
    def ratio(a: float | None, b: float | None) -> float | None:
        return None if a is None or not b else a / b
    fairness: dict[tuple[str, str], list[dict[str, str]]] = defaultdict(list)
    with (root / "fairness.csv").open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            if row["window_start_s"] == "5" and row["window_end_s"] == "30":
                pair = "+".join(sorted((row["flow_1_variant"], row["flow_2_variant"])))
                fairness[(profile_by_run.get(row["run_id"], "unknown"), pair)].append(row)
    sat_rows = []
    for rows_for_pair in fairness.values():
        for row in rows_for_pair:
            if "rudp-sat" not in (row["flow_1_variant"], row["flow_2_variant"]):
                continue
            sat = value(row, "flow_1_goodput_bps") if row["flow_1_variant"] == "rudp-sat" else value(row, "flow_2_goodput_bps")
            tcp = value(row, "flow_2_goodput_bps") if row["flow_1_variant"] == "rudp-sat" else value(row, "flow_1_goodput_bps")
            sat_rows.append((profile_by_run.get(row["run_id"], "unknown"), row["flow_1_variant"] + "+" + row["flow_2_variant"], sat or 0.0, tcp or 0.0, value(row, "jain_index")))
    cell_status: dict[tuple[str, str, str], Counter[str]] = defaultdict(Counter)
    for row in rows: cell_status[(row["profile"], row["workload"], row["variant"])][row["status"]] += 1
    lines = ["# Phase 10 RUDP-Sat compact evaluation", "", "## Frozen comparison", "",
             "This is the same compact design as Phase 9: one warm-up plus three retained blocks per cell. It compares Sat, AIMD, TCP CUBIC, TCP BBR, and raw UDP in the same randomized session; outcomes below include timeouts and failures as zero-goodput observations in the bootstrap summaries.", "",
             f"Retained outcomes: **{len(rows)}** attempts — " + ", ".join(f"{name}: {count}" for name, count in sorted(outcomes.items())) + ".", "",
             "## Predeclared sustained targets", "", "| Target | Observed ratio | Result |", "| --- | ---: | --- |",
             f"| Terrestrial Sat / raw UDP ≥ 0.90 | {ratio(terrestrial_sat, terrestrial_raw) if ratio(terrestrial_sat, terrestrial_raw) is not None else 'N/A'} | {'N/A: raw-UDP goodput aggregation unavailable' if ratio(terrestrial_sat, terrestrial_raw) is None else ('met' if ratio(terrestrial_sat, terrestrial_raw) >= .9 else 'not met')} |",
             f"| GEO Sat / TCP CUBIC > 1 | {ratio(geo_sat, geo_cubic) if ratio(geo_sat, geo_cubic) is not None else 'N/A'} | {'met' if (ratio(geo_sat, geo_cubic) or 0) > 1 else 'not met'} |",
             f"| GEO Sat / TCP BBR ≥ 0.80 | {ratio(geo_sat, geo_bbr) if ratio(geo_sat, geo_bbr) is not None else 'N/A'} | {'met' if (ratio(geo_sat, geo_bbr) or 0) >= .8 else 'not met'} |", "",
             "## Sat coexistence / TCP harm", "", "| Profile | Pair order as launched | Sat goodput bps | TCP goodput bps | Jain index |", "| --- | --- | ---: | ---: | ---: |"]
    for profile, pair, sat, tcp, jain in sat_rows:
        lines.append(f"| {profile} | {pair} | {sat:.3f} | {tcp:.3f} | {'N/A' if jain is None else f'{jain:.6f}'} |")
    lines += ["", "The complete `fairness.csv` retains all windows, capacity-step observations, and both TCP-only controls. TCP harm is reported as the companion TCP's absolute goodput and Jain index, rather than inferring fairness from Sat throughput alone.", "",
              "## Failures, queues, and limitations", "", "Every per-cell status count is in `metrics.json`/`runs.csv`; no transport outcome was excluded. Queue qdisc snapshots are retained in `counters/`. The compact collector does not reduce those snapshots to a single queue-delay scalar, so queue delay is N/A rather than fabricated. As in Phase 9, bidirectional wire-byte and retransmitted-byte aggregates were not populated; overhead is N/A, never zero. Sat remains laboratory-only: a recent SACK-loss density cannot identify physical loss cause.", ""]
    (root / "PHASE10_REPORT.md").write_text("\n".join(lines), encoding="utf-8")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
