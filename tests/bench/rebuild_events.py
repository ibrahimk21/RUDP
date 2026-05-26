#!/usr/bin/env python3
"""Rebuild an aggregate events CSV solely from immutable per-run event files."""
from __future__ import annotations

import argparse
import csv
from pathlib import Path

from harness import EVENT_FIELDS, write_csv


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("experiment", type=Path)
    args = parser.parse_args()
    rows = []
    for path in sorted((args.experiment / "logs").glob("*/events.csv")):
        with path.open(newline="", encoding="utf-8") as stream:
            rows.extend(csv.DictReader(stream))
    write_csv(args.experiment / "events.csv", EVENT_FIELDS, rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
