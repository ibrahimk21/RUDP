#!/usr/bin/env python3
"""Run a frozen Phase 9 schedule serially in disposable benchmark topologies.

This driver deliberately does not skip failed transport attempts.  It records
the first attempt for every pre-generated schedule row and stops only for an
objective harness/topology failure, leaving the immutable artifact directory
intact for diagnosis and a separately labelled follow-up session.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import subprocess
import sys
from pathlib import Path


def command_for(row: dict[str, str], experiment: Path) -> list[str]:
    root = Path(__file__).resolve().parents[2]
    workload = row["workload"]
    run_id = row["schedule_id"]
    worker = "run-fairness" if workload.startswith("fairness") else "run-pair"
    nested = [sys.executable, str(root / "tests" / "bench" / "workloads.py"), worker,
              str(experiment), run_id, row["schedule_id"], row["block_id"],
              row["profile"], workload, row["variant"]]
    topology = [sys.executable, str(root / "tests" / "bench" / "topology.py"),
                "--profile", row["profile"], "--forward-seed", row["forward_seed"],
                "--reverse-seed", row["reverse_seed"]]
    if workload.startswith("recovery_"):
        topology.append("--no-loss")
    return [*topology, "--", *nested]


def completed_rows(experiment: Path) -> set[str]:
    runs = experiment / "runs.csv"
    if not runs.exists():
        return set()
    with runs.open(newline="", encoding="utf-8") as stream:
        return {row["schedule_id"] for row in csv.DictReader(stream) if row["schedule_id"]}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("experiment", type=Path)
    parser.add_argument("--resume", action="store_true",
                        help="continue only rows with no retained attempt")
    parser.add_argument("--limit", type=int, default=0,
                        help="run at most this many rows (for a labelled pilot only)")
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error("Phase 9 topology runs require root")
    manifest_path = args.experiment / "manifest.json"
    if not manifest_path.exists():
        parser.error("experiment must be initialized with harness.py init")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("pilot"):
        parser.error("final Phase 9 driver refuses pilot artifacts")
    with (args.experiment / "schedule.csv").open(newline="", encoding="utf-8") as stream:
        schedule = list(csv.DictReader(stream))
    done = completed_rows(args.experiment)
    if done and not args.resume:
        parser.error("artifact already has retained attempts; use --resume")
    selected = [row for row in schedule if row["schedule_id"] not in done]
    if args.limit:
        selected = selected[:args.limit]
    for index, row in enumerate(selected, start=1):
        print(f"[{index}/{len(selected)}] {row['schedule_id']} {row['profile']} {row['workload']} {row['variant']}", flush=True)
        result = subprocess.run(command_for(row, args.experiment), check=False)
        retained = completed_rows(args.experiment)
        if row["schedule_id"] not in retained:
            print(f"harness/topology failure at {row['schedule_id']}: no run row retained",
                  file=sys.stderr)
            return result.returncode or 2
        if result.returncode not in (0, 1):
            print(f"harness/topology failure at {row['schedule_id']}: exit {result.returncode}", file=sys.stderr)
            return result.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
