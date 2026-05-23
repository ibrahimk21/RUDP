#!/usr/bin/env python3
"""Refuse experimental collection until every BENCHMARKING.md gate passes."""
from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

from harness import load_config
from validation import parse_tc, parse_tcpdump, validate_leo, validate_offloads, validate_packets, validate_queues, validate_random_loss, validate_rtt, validate_saturation


def csv_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def probe_sets(root: Path, profile: str, direction: str) -> list[list[dict[str, str]]]:
    paths = sorted(root.glob(f"{profile}/*/{direction}.csv"))
    if not paths:
        raise ValueError(f"missing {profile} {direction} probes")
    return [csv_rows(path) for path in paths]


def main() -> int:
    parser = argparse.ArgumentParser(); parser.add_argument("validation_root", type=Path); parser.add_argument("output", type=Path); args = parser.parse_args()
    project = Path(__file__).resolve().parents[2]; config = load_config(project / "tests" / "bench" / "config.json")
    checks: dict[str, Any] = {}
    try:
        for direction in ("forward", "reverse"):
            baseline = [float(row["rtt_ms"]) for trial in probe_sets(args.validation_root, "calibration_baseline", direction) for row in trial if row["rtt_ms"]][:100]
            for profile in ("terrestrial", "geo", "leo"):
                trials = probe_sets(args.validation_root, profile, direction)
                rtts = [float(row["rtt_ms"]) for row in trials[0] if row["rtt_ms"]][:100]
                checks[f"rtt:{profile}:{direction}"] = validate_rtt(baseline, rtts, config["profiles"][profile]["delay_ms"])
                received = [[row["received"] == "1" for row in trial[:10000]] for trial in trials]
                loss = config["profiles"][profile]["loss"]
                if profile == "leo":
                    checks[f"loss:{profile}:{direction}"] = validate_leo(received)
                else:
                    expected = loss.get("percent", 0) / 100
                    checks[f"loss:{profile}:{direction}"] = validate_random_loss(received[0], expected)
        feature_files = sorted(args.validation_root.glob("*/*/offloads.json"))
        combined_features = {}
        for path in feature_files:
            combined_features.update(json.loads(path.read_text()))
        checks["offloads"] = validate_offloads(combined_features)
        saturation = json.loads((args.validation_root / "saturation" / "saturation.json").read_text())
        checks["saturation"] = validate_saturation(int(saturation["delivered_bytes"]), float(saturation["duration_s"]), float(saturation["expected_rate_bps"]))
        capture_files = sorted((args.validation_root / "captures").glob("*.pcap")) + sorted((args.validation_root / "saturation").glob("*.pcap"))
        packets = []
        for path in capture_files:
            text = subprocess.run(["tcpdump", "-tt", "-nn", "-r", str(path)], check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
            packets.extend(parse_tcpdump(text))
        tcp_statuses = []
        for path in args.validation_root.glob("tcp-*.jsonl"):
            for line in path.read_text().splitlines():
                if line.startswith("{"):
                    row = json.loads(line)
                    if "geo_mss_1024" in path.stem:
                        row["profile"] = "geo_mss_1024"
                    tcp_statuses.append(row)
        algorithms = {row.get("cc_actual") for row in tcp_statuses if row.get("status") == "success"}
        checks["tcp_algorithms"] = {"passed": {"cubic", "bbr"}.issubset(algorithms), "observed": sorted(value for value in algorithms if value)}
        checks["packet_sizes"] = validate_packets(packets, [int(row.get("tcp_snd_mss", 0)) for row in tcp_statuses])
        matched = [row for row in tcp_statuses if row.get("profile") == "geo_mss_1024"]
        checks["matched_mss"] = validate_packets(packets, [int(row.get("tcp_snd_mss", 0)) for row in matched], 1024)
        queue_rows = json.loads((args.validation_root / "queues.json").read_text())
        checks["queues"] = validate_queues(queue_rows, config["topology"]["netem_limit_packets"])
        cleanup = json.loads((args.validation_root / "cleanup.json").read_text())
        checks["failure_cleanup"] = {"passed": bool(cleanup.get("failure_injected")) and not cleanup.get("remaining_owned_namespaces") and not cleanup.get("remaining_owned_manifests"), **cleanup}
        failed = json.loads((args.validation_root / "intentional-failure.json").read_text())
        checks["failed_run_visible"] = {"passed": failed.get("status") in ("failed", "timeout") and failed.get("excluded") in (0, False), "row": failed}
    except (OSError, ValueError, KeyError, json.JSONDecodeError, subprocess.CalledProcessError) as error:
        checks["collection_complete"] = {"passed": False, "reason": str(error)}
    passed = bool(checks) and all(check.get("passed") is True for check in checks.values())
    report = {"schema_version": 1, "passed": passed, "collection_permitted": passed, "checks": checks}
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print("pre-collection validation passed" if passed else "pre-collection validation FAILED", file=sys.stdout if passed else sys.stderr)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
