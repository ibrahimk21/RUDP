#!/usr/bin/env python3
"""Convert raw tc/link snapshots to the queue gate's auditable rows."""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

from validation import parse_tc


def main() -> int:
    parser = argparse.ArgumentParser(); parser.add_argument("output", type=Path); parser.add_argument("snapshots", nargs="+", type=Path); args = parser.parse_args()
    rows = []
    for path in args.snapshots:
        snapshot = json.loads(path.read_text())
        for namespace, values in snapshot["namespaces"].items():
            qdisc = values.get("qdisc", "")
            blocks = re.split(r"(?=^qdisc )", qdisc, flags=re.MULTILINE)
            for block in blocks:
                if not block.startswith("qdisc "):
                    continue
                parsed = parse_tc(block)
                parsed.update({"timestamp_ns": snapshot["timestamp_ns"], "phase": snapshot["phase"], "namespace": namespace, "kind": "propagation" if block.startswith("qdisc netem") else "bottleneck" if block.startswith("qdisc bfifo") else "other", "endpoint_drops": 0})
                rows.append(parsed)
            try:
                links = json.loads(values.get("links", "[]"))
            except json.JSONDecodeError:
                links = []
            if namespace.endswith(("s", "r")):
                endpoint_drops = sum(int(item.get("stats64", {}).get("rx", {}).get("dropped", 0)) + int(item.get("stats64", {}).get("tx", {}).get("dropped", 0)) for item in links)
                rows.append({"timestamp_ns": snapshot["timestamp_ns"], "phase": snapshot["phase"], "namespace": namespace, "kind": "endpoint", "backlog_bytes": 0, "backlog_packets": 0, "drops": 0, "overlimits": 0, "requeues": 0, "endpoint_drops": endpoint_drops})
    args.output.write_text(json.dumps(rows, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
