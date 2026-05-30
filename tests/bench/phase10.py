#!/usr/bin/env python3
"""Run a frozen Phase 10 schedule, including explicitly labelled pilots."""
from __future__ import annotations

import argparse
import csv
import json
import os
import subprocess
import sys
from pathlib import Path

from phase9 import command_for, completed_rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("experiment", type=Path)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--limit", type=int, default=0,
                        help="required for a pilot; bounded subset for a final dry run")
    parser.add_argument("--only-variant")
    parser.add_argument("--only-profile")
    parser.add_argument("--only-workload")
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error("Phase 10 topology runs require root")
    manifest_path = args.experiment / "manifest.json"
    if not manifest_path.exists():
        parser.error("experiment must be initialized with harness.py init")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("pilot") and args.limit <= 0:
        parser.error("a pilot must declare a nonzero --limit")
    with (args.experiment / "schedule.csv").open(newline="", encoding="utf-8") as stream:
        schedule = list(csv.DictReader(stream))
    done = completed_rows(args.experiment)
    if done and not args.resume:
        parser.error("artifact already has retained attempts; use --resume")
    selected = [row for row in schedule if row["schedule_id"] not in done]
    for field, value in (("variant", args.only_variant), ("profile", args.only_profile),
                         ("workload", args.only_workload)):
        if value:
            selected = [row for row in selected if row[field] == value]
    if args.limit:
        selected = selected[:args.limit]
    if not selected:
        parser.error("no schedule rows match the requested selection")
    for index, row in enumerate(selected, start=1):
        print(f"[{index}/{len(selected)}] {row['schedule_id']} {row['profile']} "
              f"{row['workload']} {row['variant']}", flush=True)
        result = subprocess.run(command_for(row, args.experiment), check=False)
        if row["schedule_id"] not in completed_rows(args.experiment):
            print(f"harness/topology failure at {row['schedule_id']}: no run row retained",
                  file=sys.stderr)
            return result.returncode or 2
        if result.returncode not in (0, 1):
            return result.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
