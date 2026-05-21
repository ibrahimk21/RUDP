#!/usr/bin/env python3
"""Auditable Phase 8 scheduling, artifact schemas, metrics, and command execution."""
from __future__ import annotations

import csv
import hashlib
import json
import math
import os
import platform
import random
import resource
import shlex
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Iterable

SCHEMA_VERSION = 1
SCHEDULE_FIELDS = ["schedule_id", "session_id", "block_id", "warmup", "profile", "workload", "variant", "forward_seed", "reverse_seed", "generator_seed", "order"]
RUN_FIELDS = ["schema_version", "run_id", "manifest_id", "schedule_id", "session_id", "block_id", "warmup", "profile", "workload", "variant", "status", "error", "excluded", "exclusion_reason", "started_ns", "measurement_start_ns", "measurement_end_ns", "ended_ns", "offered_bytes", "delivered_bytes", "wire_bytes", "retransmitted_bytes", "duration_s", "goodput_bps", "latency_p50_ms", "latency_p95_ms", "latency_p99_ms", "latency_samples", "undelivered_records", "setup_ms", "first_byte_ms", "recovery_ms", "sender_user_cpu_s", "sender_system_cpu_s", "receiver_user_cpu_s", "receiver_system_cpu_s", "digest_ok", "records_path", "events_path", "queue_path", "counters_path"]
RECORD_FIELDS = ["run_id", "flow_id", "record_id", "offer_ns", "accepted_ns", "delivered_ns", "bytes", "valid", "duplicate"]
EVENT_FIELDS = ["run_id", "timestamp_ns", "kind", "direction", "packet_id", "record_id", "bytes", "detail"]
QUEUE_FIELDS = ["run_id", "timestamp_ns", "namespace", "interface", "backlog_bytes", "backlog_packets", "drops", "overlimits", "requeues"]
FAIRNESS_FIELDS = ["run_id", "flow_1_id", "flow_1_variant", "flow_2_id", "flow_2_variant", "window_start_s", "window_end_s", "flow_1_goodput_bps", "flow_2_goodput_bps", "jain_index", "utilization_bps"]


def load_config(path: Path) -> dict[str, Any]:
    config = json.loads(path.read_text(encoding="utf-8"))
    required = {"schema_version", "final_seed", "pilot_seed", "topology", "profiles", "variants", "coexistence_pairs", "workloads", "sampling", "calibration", "raw_udp"}
    if set(config) != required or config["schema_version"] != SCHEMA_VERSION:
        raise ValueError("unsupported or incomplete benchmark configuration")
    if config["sampling"]["retained_blocks"] != 10 or config["sampling"]["warmups"] != 1:
        raise ValueError("BENCHMARKING.md requires one warm-up and ten retained blocks")
    if config["topology"]["fifo_bytes"] != config["topology"]["rate_bps"] * 50 // 8000:
        raise ValueError("FIFO must hold exactly 50 ms at the configured rate")
    return config


def _rotate(items: list[str], count: int) -> list[str]:
    count %= len(items)
    return items[count:] + items[:count]


def generate_schedule(config: dict[str, Any], pilot: bool = False) -> list[dict[str, Any]]:
    """Generate all choices before execution; balance variant positions by rotation."""
    rng = random.Random(config["pilot_seed"] if pilot else config["final_seed"])
    profiles = list(config["profiles"])
    variants = list(config["variants"])
    rows: list[dict[str, Any]] = []
    blocks = config["sampling"]["warmups"] + config["sampling"]["retained_blocks"]
    for block in range(blocks):
        profile_order = profiles[:]
        rng.shuffle(profile_order)
        variant_order = _rotate(variants, block)
        if block % len(variants) == 0:
            rng.shuffle(variant_order)
        order = 0
        for profile in profile_order:
            forward_seed = rng.randrange(1, 2**31)
            reverse_seed = rng.randrange(1, 2**31)
            sensitivity = bool(config["profiles"][profile].get("sensitivity"))
            only = config["profiles"][profile].get("workload_only")
            workloads = [only] if only else (["sustained"] if sensitivity else [name for name in config["workloads"] if name != "fairness_step"])
            for workload in workloads:
                choices = config["coexistence_pairs"] if workload.startswith("fairness") else variant_order
                for variant in choices:
                    if variant == "raw-udp" and workload != "sustained":
                        continue
                    if workload.startswith("fairness") and profile not in ("terrestrial", "geo", "leo", "geo_congestion", "geo_capacity_step"):
                        continue
                    order += 1
                    rows.append({
                        "schedule_id": f"b{block:02d}-{order:04d}", "session_id": "session-01",
                        "block_id": block, "warmup": int(block == 0), "profile": profile,
                        "workload": workload, "variant": variant,
                        "forward_seed": forward_seed, "reverse_seed": reverse_seed,
                        "generator_seed": rng.randrange(1, 2**31), "order": order,
                    })
    return rows


def write_csv(path: Path, fields: list[str], rows: Iterable[dict[str, Any]] = ()) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def percentile(values: list[float], probability: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = (len(ordered) - 1) * probability
    lower = math.floor(index)
    upper = math.ceil(index)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def latency_metrics(records: Iterable[dict[str, Any]]) -> dict[str, Any]:
    rows = list(records)
    latencies = [(int(row["delivered_ns"]) - int(row["offer_ns"])) / 1e6 for row in rows if row.get("delivered_ns") not in (None, "")]
    return {"latency_p50_ms": percentile(latencies, .50), "latency_p95_ms": percentile(latencies, .95), "latency_p99_ms": percentile(latencies, .99), "latency_samples": len(latencies), "undelivered_records": len(rows) - len(latencies)}


def jain_index(first: float, second: float) -> float | None:
    denominator = 2.0 * (first * first + second * second)
    return None if denominator == 0 else (first + second) ** 2 / denominator


def window_goodput(records: Iterable[dict[str, Any]], start_ns: int, end_ns: int) -> float:
    if end_ns <= start_ns:
        raise ValueError("invalid measurement window")
    delivered = sum(int(row["bytes"]) for row in records if row.get("delivered_ns") not in (None, "") and start_ns <= int(row["delivered_ns"]) < end_ns and str(row.get("duplicate", "0")) in ("0", "False", "false", ""))
    return delivered * 8e9 / (end_ns - start_ns)


def setup_metrics(started_ns: int, ready_ns: int, first_byte_ns: int | None) -> dict[str, float | None]:
    if ready_ns < started_ns or (first_byte_ns is not None and first_byte_ns < started_ns):
        raise ValueError("non-monotonic setup trace")
    return {"setup_ms": (ready_ns - started_ns) / 1e6, "first_byte_ms": None if first_byte_ns is None else (first_byte_ns - started_ns) / 1e6}


def recovery_metrics(events: Iterable[dict[str, Any]], records: Iterable[dict[str, Any]], target: int) -> dict[str, Any]:
    drops = [row for row in events if row.get("kind") == "controlled_drop" and int(row.get("record_id", -1)) == target]
    delivered = [row for row in records if int(row["record_id"]) == target and row.get("delivered_ns") not in (None, "")]
    later = [row for row in records if int(row["record_id"]) > target and row.get("delivered_ns") not in (None, "")]
    if len(drops) != 1:
        raise ValueError("controlled recovery requires exactly one proved drop")
    injected = int(drops[0]["timestamp_ns"])
    return {"drop_count": 1, "recovery_ms": None if not delivered else (int(delivered[0]["delivered_ns"]) - injected) / 1e6, "later_data": bool(later), "censored": not bool(delivered)}


def fairness_metrics(flow_rows: dict[str, list[dict[str, Any]]], windows: Iterable[tuple[int, int]], t0_ns: int) -> list[dict[str, Any]]:
    if set(flow_rows) != {"flow-1", "flow-2"}:
        raise ValueError("fairness needs exactly two named flows")
    output = []
    for start_s, end_s in windows:
        first = window_goodput(flow_rows["flow-1"], t0_ns + start_s * 10**9, t0_ns + end_s * 10**9)
        second = window_goodput(flow_rows["flow-2"], t0_ns + start_s * 10**9, t0_ns + end_s * 10**9)
        output.append({"window_start_s": start_s, "window_end_s": end_s, "flow_1_goodput_bps": first, "flow_2_goodput_bps": second, "jain_index": jain_index(first, second), "utilization_bps": first + second})
    return output


def bootstrap_ratio(blocks: list[tuple[float, float]], resamples: int, seed: int) -> tuple[float | None, float | None, float | None]:
    if not blocks:
        return None, None, None
    numerator = statistics.mean(item[0] for item in blocks)
    denominator = statistics.mean(item[1] for item in blocks)
    if denominator == 0:
        return None, None, None
    rng = random.Random(seed)
    samples: list[float] = []
    for _ in range(resamples):
        chosen = [blocks[rng.randrange(len(blocks))] for _ in blocks]
        base = statistics.mean(item[1] for item in chosen)
        if base != 0:
            samples.append(statistics.mean(item[0] for item in chosen) / base)
    return numerator / denominator, percentile(samples, .025), percentile(samples, .975)


def bootstrap_mean(values: list[float], resamples: int, seed: int) -> tuple[float | None, float | None, float | None]:
    if not values:
        return None, None, None
    rng = random.Random(seed)
    samples = [statistics.mean(values[rng.randrange(len(values))] for _ in values) for _ in range(resamples)]
    return statistics.mean(values), percentile(samples, .025), percentile(samples, .975)


def create_artifacts(root: Path, experiment_id: str, config_path: Path, pilot: bool = False) -> Path:
    output = root / experiment_id
    output.mkdir(parents=True, exist_ok=False)
    config = load_config(config_path)
    config_bytes = config_path.read_bytes()
    manifest = {
        "schema_version": SCHEMA_VERSION, "manifest_id": experiment_id, "pilot": pilot,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "config_sha256": hashlib.sha256(config_bytes).hexdigest(), "config": config,
        "repository": repository_state(config_path.parent.parent.parent),
        "environment": {"platform": platform.platform(), "python": sys.version, "argv": sys.argv},
        "reproduce": [f"python3 tests/bench/harness.py init {shlex.quote(experiment_id)}"],
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_csv(output / "schedule.csv", SCHEDULE_FIELDS, generate_schedule(config, pilot))
    for name, fields in (("runs.csv", RUN_FIELDS), ("records.csv", RECORD_FIELDS), ("events.csv", EVENT_FIELDS), ("queue.csv", QUEUE_FIELDS), ("fairness.csv", FAIRNESS_FIELDS)):
        write_csv(output / name, fields)
    (output / "counters").mkdir()
    (output / "logs").mkdir()
    return output


def repository_state(root: Path) -> dict[str, Any]:
    def git(*args: str) -> str:
        return subprocess.run(["git", *args], cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False).stdout.strip()
    return {"revision": git("rev-parse", "HEAD"), "status": git("status", "--porcelain"), "diff": git("diff", "--binary")}


def run_command(command: list[str], timeout_s: float, stdout_path: Path, stderr_path: Path) -> dict[str, Any]:
    """Run one attempt with a hard deadline and child CPU accounting; never erase failure."""
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    started = time.monotonic_ns()
    status, error, code = "success", "", 0
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        try:
            process = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=timeout_s, check=False)
            code = process.returncode
            if code != 0:
                status, error = "failed", f"exit {code}"
        except subprocess.TimeoutExpired:
            status, error, code = "timeout", f"deadline {timeout_s:g}s exceeded", 124
    ended = time.monotonic_ns()
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    return {"status": status, "error": error, "exit_code": code, "started_ns": started, "ended_ns": ended,
            "user_cpu_s": after.ru_utime - before.ru_utime, "system_cpu_s": after.ru_stime - before.ru_stime}


def snapshot_counters(output: Path, namespaces: list[str], phase: str) -> None:
    data: dict[str, Any] = {"timestamp_ns": time.monotonic_ns(), "phase": phase, "namespaces": {}}
    for namespace in namespaces:
        commands = {"qdisc": ["ip", "netns", "exec", namespace, "tc", "-s", "qdisc", "show"], "class": ["ip", "netns", "exec", namespace, "tc", "-s", "class", "show"], "links": ["ip", "-j", "-n", namespace, "-s", "link", "show"]}
        data["namespaces"][namespace] = {key: subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False).stdout for key, cmd in commands.items()}
    output.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: harness.py {validate|schedule|init|run} ...", file=sys.stderr); return 2
    root = Path(__file__).resolve().parents[2]
    config_path = root / "tests" / "bench" / "config.json"
    command = sys.argv[1]
    if command == "validate":
        config = load_config(config_path); print(f"benchmark configuration v{config['schema_version']} is valid"); return 0
    if command == "schedule":
        writer = csv.DictWriter(sys.stdout, fieldnames=SCHEDULE_FIELDS); writer.writeheader(); writer.writerows(generate_schedule(load_config(config_path), "--pilot" in sys.argv)); return 0
    if command == "init" and len(sys.argv) in (3, 4):
        path = create_artifacts(root / "results", sys.argv[2], config_path, len(sys.argv) == 4 and sys.argv[3] == "--pilot"); print(path); return 0
    if command == "run" and len(sys.argv) >= 6:
        result = run_command(sys.argv[4:], float(sys.argv[3]), Path(sys.argv[2] + ".out"), Path(sys.argv[2] + ".err")); print(json.dumps(result, sort_keys=True)); return 0 if result["status"] == "success" else 1
    print("invalid arguments", file=sys.stderr); return 2


if __name__ == "__main__":
    raise SystemExit(main())
