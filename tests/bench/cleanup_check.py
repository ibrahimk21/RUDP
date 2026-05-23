#!/usr/bin/env python3
"""Prove that an injected controller failure leaves no owned resources."""
from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path


def namespaces() -> set[str]:
    output = subprocess.run(["ip", "netns", "list"], check=True, text=True, stdout=subprocess.PIPE).stdout
    return {line.split()[0] for line in output.splitlines() if line.strip()}


def main() -> int:
    parser = argparse.ArgumentParser(); parser.add_argument("output", type=Path); args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    before = namespaces()
    before_manifests = set((root / "results" / "_work").glob("rudp*.topology.json"))
    process = subprocess.run([str(root / "tests" / "bench" / "topology.py"), "--profile", "calibration_baseline", "--", "sh", "-c", "exit 23"], check=False)
    after = namespaces(); leaked = sorted(name for name in after - before if name.startswith("rudp"))
    stale_manifests = sorted(str(path) for path in set((root / "results" / "_work").glob("rudp*.topology.json")) - before_manifests)
    result = {"failure_injected": process.returncode == 23, "child_exit_code": process.returncode, "remaining_owned_namespaces": leaked, "remaining_owned_manifests": stale_manifests, "passed": process.returncode == 23 and not leaked and not stale_manifests}
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
