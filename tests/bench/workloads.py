#!/usr/bin/env python3
"""Execute Phase 8 workload attempts inside topology.py's owned namespaces."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import signal
import subprocess
import time
from pathlib import Path
from typing import Any

from harness import EVENT_FIELDS, FAIRNESS_FIELDS, RECORD_FIELDS, RUN_FIELDS, fairness_metrics, latency_metrics, load_config, recovery_metrics, setup_metrics, snapshot_counters, window_goodput, write_csv


def make_record(record_id: int, offer_ns: int = 0) -> bytes:
    header = record_id.to_bytes(8, "big") + offer_ns.to_bytes(8, "big")
    return header + bytes((record_id * 31 + index * 17) & 255 for index in range(16, 1024))


def prepare_file(path: Path, size: int, records: bool = False) -> str:
    digest = hashlib.md5(usedforsecurity=False)
    with path.open("wb") as stream:
        offset = 0
        while offset < size:
            chunk = make_record(offset // 1024) if records else bytes(((offset + index) * 17 + 3) & 255 for index in range(min(65536, size - offset)))
            chunk = chunk[:size - offset]
            stream.write(chunk); digest.update(chunk); offset += len(chunk)
    with path.open("rb") as stream:
        while stream.read(1024 * 1024):
            pass
    return digest.hexdigest()


def parse_status(path: Path) -> dict[str, Any]:
    for line in reversed(path.read_text(encoding="utf-8", errors="replace").splitlines()):
        try:
            value = json.loads(line)
            if isinstance(value, dict) and "status" in value:
                return value
        except json.JSONDecodeError:
            pass
    return {"status": "failure", "error": "missing endpoint status", "started_ns": 0, "ready_ns": 0, "first_byte_ns": 0, "ended_ns": 0, "bytes": 0}


def read_records(path: Path, run_id: str, flow_id: str = "flow-1") -> list[dict[str, Any]]:
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8") as stream:
        return [{"run_id": run_id, "flow_id": flow_id, "record_id": row["record_id"], "offer_ns": row["offer_ns"], "accepted_ns": row["offer_ns"], "delivered_ns": row["delivered_ns"], "bytes": 1024, "valid": 1, "duplicate": 0} for row in csv.DictReader(stream)]


def endpoint(root: Path, namespace: str, command: list[str], log: Path, environment: dict[str, str] | None = None) -> subprocess.Popen[bytes]:
    wrapper = root / "tests" / "bench" / "run_in_namespace.sh"
    output = log.open("wb")
    return subprocess.Popen([str(wrapper), namespace, *command], stdout=output, stderr=subprocess.STDOUT, env=environment, start_new_session=True)


def wait_all(processes: list[subprocess.Popen[bytes]], deadline_s: float) -> bool:
    deadline = time.monotonic() + deadline_s
    while time.monotonic() < deadline:
        if all(process.poll() is not None for process in processes):
            return True
        time.sleep(.02)
    for process in processes:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
    time.sleep(.1)
    for process in processes:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
    return False


def commands(root: Path, variant: str, workload: str, port: int, input_path: Path, output_path: Path, config: dict[str, Any]) -> tuple[list[str], list[str]]:
    host = "203.0.113.2"
    if variant == "rudp-aimd":
        receiver = [str(root / "build" / "rudp"), "receive", str(port), "-" if workload in ("sustained", "latency") else str(output_path)]
        if workload in ("sustained", "latency"):
            sender = [str(root / "build" / "rudp"), "latency" if workload == "latency" else "stream", host, str(port), str(config["workloads"][workload]["duration_s"] * 1000)]
        else:
            sender = [str(root / "build" / "rudp"), "send", host, str(port), str(input_path)]
        return receiver, sender
    if variant.startswith("tcp-"):
        cc = variant.removeprefix("tcp-")
        receiver = [str(root / "build" / "tcp_ref"), "receive", str(port), "-" if workload in ("sustained", "latency") else str(output_path), cc]
        if workload in ("sustained", "latency"):
            sender = [str(root / "build" / "tcp_ref"), "latency" if workload == "latency" else "stream", host, str(port), str(config["workloads"][workload]["duration_s"] * 1000), cc]
        else:
            sender = [str(root / "build" / "tcp_ref"), "send", host, str(port), str(input_path), cc]
        return receiver, sender
    if variant == "raw-udp" and workload == "sustained":
        count = config["raw_udp"]["rate_bps"] * config["workloads"]["sustained"]["duration_s"] // (8 * config["raw_udp"]["ip_udp_record_bytes"])
        receiver = [str(root / "build" / "udp_ref"), "receive", str(port), str(port + 1)]
        sender = [str(root / "build" / "udp_ref"), "send", host, str(port), str(port + 1), str(count), str(config["raw_udp"]["rate_bps"])]
        return receiver, sender
    raise ValueError(f"unsupported workload/variant: {workload}/{variant}")


def append_rows(path: Path, fields: list[str], rows: list[dict[str, Any]]) -> None:
    existing: list[dict[str, Any]] = []
    if path.exists():
        with path.open(newline="", encoding="utf-8") as stream:
            existing = list(csv.DictReader(stream))
    write_csv(path, fields, [*existing, *rows])


def endpoint_writable(path: Path) -> None:
    uid = int(os.environ["RUDP_RUN_AS_UID"]); gid = int(os.environ["RUDP_RUN_AS_GID"])
    path.mkdir(parents=True, exist_ok=True); os.chown(path, uid, gid); path.chmod(0o770)


def run_pair(args: argparse.Namespace, root: Path, config: dict[str, Any]) -> int:
    if args.workload.startswith("recovery_") and os.environ.get("RUDP_BACKGROUND_LOSS") != "0":
        raise SystemExit("controlled-loss workloads require topology.py --no-loss")
    experiment = args.experiment.resolve(); run_dir = experiment / "logs" / args.run_id; run_dir.mkdir(parents=True, exist_ok=False); endpoint_writable(run_dir)
    input_path, output_path = run_dir / "input.bin", run_dir / "output.bin"
    specification = config["workloads"][args.workload]
    digest = ""
    if args.workload.startswith("file_"):
        digest = prepare_file(input_path, specification["bytes"])
    elif args.workload.startswith("recovery_"):
        digest = prepare_file(input_path, specification["records"] * 1024, records=True)
    receiver_command, sender_command = commands(root, args.variant, args.workload, args.port, input_path, output_path, config)
    record_path = run_dir / "receiver-records.csv"
    environment = os.environ.copy()
    if args.workload in ("sustained", "latency") or args.workload.startswith("recovery_"):
        environment["RUDP_RECORDS_CSV"] = str(record_path)
    if "tcp_maxseg" in config["profiles"][args.profile]:
        environment["RUDP_TCP_MAXSEG"] = str(config["profiles"][args.profile]["tcp_maxseg"])
    counter_dir = experiment / "counters"; namespaces = [os.environ[name] for name in ("RUDP_SENDER_NS", "RUDP_SHAPER_NS", "RUDP_DELAY_NS", "RUDP_RECEIVER_NS")]
    snapshot_counters(counter_dir / f"{args.run_id}-before.json", namespaces, "before")
    capture = subprocess.Popen(["ip", "netns", "exec", os.environ["RUDP_DELAY_NS"], "tcpdump", "-U", "-n", "-i", "d1", "-w", str(run_dir / "capture.pcap"), "port", str(args.port)], stdout=subprocess.DEVNULL, stderr=(run_dir / "tcpdump.log").open("wb"))
    if args.workload.startswith("recovery_"):
        protocol = "tcp" if args.variant.startswith("tcp-") else "rudp"
        subprocess.run([str(root / "tests" / "bench" / "one_shot_drop.py"), os.environ["RUDP_DELAY_NS"], "d1", protocol, str(args.port), str(specification["drop_record"]), str(run_dir / "drop.json")], check=True)
    receiver = endpoint(root, os.environ["RUDP_RECEIVER_NS"], receiver_command, run_dir / "receiver.log", environment)
    time.sleep(.1)
    sender = endpoint(root, os.environ["RUDP_SENDER_NS"], sender_command, run_dir / "sender.log", environment)
    deadline = specification.get("deadline_s", specification.get("duration_s", 120) + specification.get("drain_deadline_s", 120))
    completed = wait_all([sender, receiver], deadline)
    capture.send_signal(signal.SIGINT); capture.wait(timeout=5)
    snapshot_counters(counter_dir / f"{args.run_id}-after.json", namespaces, "after")
    sender_status, receiver_status = parse_status(run_dir / "sender.log"), parse_status(run_dir / "receiver.log")
    records = read_records(record_path, args.run_id)
    status = "success" if completed and sender.returncode == 0 and receiver.returncode == 0 else ("timeout" if not completed else "failed")
    row = {field: "" for field in RUN_FIELDS}; row.update({"schema_version": 1, "run_id": args.run_id, "manifest_id": json.loads((experiment / "manifest.json").read_text())["manifest_id"], "schedule_id": args.schedule_id, "session_id": "session-01", "block_id": args.block_id, "warmup": int(args.block_id == 0), "profile": args.profile, "workload": args.workload, "variant": args.variant, "status": status, "error": "" if status == "success" else ("deadline exceeded" if status == "timeout" else f"sender={sender.returncode},receiver={receiver.returncode}"), "excluded": 0, "started_ns": sender_status.get("started_ns", 0), "ended_ns": max(int(sender_status.get("ended_ns", 0)), int(receiver_status.get("ended_ns", 0))), "offered_bytes": sender_status.get("bytes", 0), "delivered_bytes": receiver_status.get("bytes", 0), "sender_user_cpu_s": int(sender_status.get("user_cpu_ns", 0)) / 1e9, "sender_system_cpu_s": int(sender_status.get("system_cpu_ns", 0)) / 1e9, "receiver_user_cpu_s": int(receiver_status.get("user_cpu_ns", 0)) / 1e9, "receiver_system_cpu_s": int(receiver_status.get("system_cpu_ns", 0)) / 1e9, "digest_ok": int(status == "success" and (not digest or output_path.exists() and hashlib.md5(output_path.read_bytes(), usedforsecurity=False).hexdigest() == digest)), "records_path": str(record_path.relative_to(experiment)), "events_path": f"logs/{args.run_id}/events.csv", "counters_path": f"counters/{args.run_id}-after.json"})
    if records:
        t0 = int(records[0]["offer_ns"])
        if args.workload == "sustained":
            start = t0 + config["workloads"]["sustained"]["settle_s"] * 10**9; end = start + config["workloads"]["sustained"]["measure_s"] * 10**9
            row.update({"measurement_start_ns": start, "measurement_end_ns": end, "duration_s": 90, "goodput_bps": window_goodput(records, start, end) if status == "success" else 0})
        row.update(latency_metrics(records))
    ready, first = int(sender_status.get("ready_ns", 0)), int(receiver_status.get("first_byte_ns", 0))
    if ready:
        row.update(setup_metrics(int(sender_status["started_ns"]), ready, first or None))
    events: list[dict[str, Any]] = []
    if args.workload.startswith("recovery_"):
        subprocess.run([str(root / "tests" / "bench" / "one_shot_drop.py"), os.environ["RUDP_DELAY_NS"], "d1", "tcp" if args.variant.startswith("tcp-") else "rudp", str(args.port), str(specification["drop_record"]), str(run_dir / "drop-evidence.json"), "--verify"], check=True)
        evidence = json.loads((run_dir / "drop-evidence.json").read_text()); events = [{"run_id": args.run_id, "timestamp_ns": evidence["injection_timestamp_ns"], "kind": "controlled_drop", "direction": "forward", "packet_id": evidence["packet_sequence"], "record_id": specification["drop_record"], "bytes": evidence["packet_payload_bytes"], "detail": "one-shot tc/eBPF"}]
        row.update(recovery_metrics(events, records, specification["drop_record"]))
    write_csv(run_dir / "events.csv", EVENT_FIELDS, events); append_rows(experiment / "records.csv", RECORD_FIELDS, records); append_rows(experiment / "events.csv", EVENT_FIELDS, events); append_rows(experiment / "runs.csv", RUN_FIELDS, [row])
    return 0 if status == "success" else 1


def run_fairness(args: argparse.Namespace, root: Path, config: dict[str, Any]) -> int:
    experiment = args.experiment.resolve(); run_dir = experiment / "logs" / args.run_id; run_dir.mkdir(parents=True, exist_ok=False); endpoint_writable(run_dir)
    variants = args.variant.split("+")
    if len(variants) != 2:
        raise ValueError("fairness variant must name two flows")
    if args.block_id % 2:
        variants.reverse()
    barrier = run_dir / "barrier"; endpoint_writable(barrier)
    processes: list[subprocess.Popen[bytes]] = []
    senders: list[subprocess.Popen[bytes]] = []
    record_paths: list[Path] = []
    for index, variant in enumerate(variants):
        port = args.port + index * 10; records = run_dir / f"flow-{index + 1}-records.csv"; records.touch(); os.chown(records, int(os.environ["RUDP_RUN_AS_UID"]), int(os.environ["RUDP_RUN_AS_GID"])); record_paths.append(records)
        receiver_command, sender_command = commands(root, variant, "sustained", port, run_dir / "unused", run_dir / "unused-output", config)
        environment = os.environ.copy(); environment["RUDP_RECORDS_CSV"] = str(records)
        processes.append(endpoint(root, os.environ["RUDP_RECEIVER_NS"], receiver_command, run_dir / f"flow-{index + 1}-receiver.log", environment))
        barrier_command = [str(root / "tests" / "bench" / "barrier_exec.py"), str(barrier), f"flow-{index + 1}", "2", "--", *sender_command]
        sender = endpoint(root, os.environ["RUDP_SENDER_NS"], barrier_command, run_dir / f"flow-{index + 1}-sender.log")
        senders.append(sender); processes.append(sender)
    t0_ns = time.monotonic_ns(); duration = config["workloads"][args.workload]["duration_s"]
    stepped = False
    for second in range(duration + 1):
        target = t0_ns + second * 10**9
        delay = (target - time.monotonic_ns()) / 1e9
        if delay > 0:
            time.sleep(delay)
        snapshot_counters(experiment / "counters" / f"{args.run_id}-{second:03d}.json", [os.environ["RUDP_SHAPER_NS"], os.environ["RUDP_DELAY_NS"]], f"second-{second}")
        if args.workload == "fairness_step" and not stepped and second >= config["workloads"]["fairness_step"]["step_at_s"]:
            rate = config["workloads"]["fairness_step"]["step_rate_bps"]; fifo = config["workloads"]["fairness_step"]["step_fifo_bytes"]
            subprocess.run(["ip", "netns", "exec", os.environ["RUDP_SHAPER_NS"], "tc", "class", "change", "dev", "h1", "parent", "1:", "classid", "1:10", "htb", "rate", f"{rate}bit", "ceil", f"{rate}bit"], check=True)
            subprocess.run(["ip", "netns", "exec", os.environ["RUDP_SHAPER_NS"], "tc", "qdisc", "change", "dev", "h1", "parent", "1:10", "handle", "10:", "bfifo", "limit", str(fifo)], check=True)
            stepped = True
    completed = wait_all(processes, config["workloads"]["sustained"]["drain_deadline_s"])
    status = "success" if completed and all(process.returncode == 0 for process in processes) else ("timeout" if not completed else "failed")
    flow_rows = {f"flow-{index + 1}": read_records(record_paths[index], args.run_id, f"flow-{index + 1}") for index in range(2)}
    first_offers = [int(rows[0]["offer_ns"]) for rows in flow_rows.values() if rows]
    common_t0 = max(first_offers) if len(first_offers) == 2 else t0_ns
    metrics = fairness_metrics(flow_rows, [tuple(window) for window in config["workloads"][args.workload]["windows_s"]], common_t0)
    fairness_rows = [{"run_id": args.run_id, "flow_1_id": "flow-1", "flow_1_variant": variants[0], "flow_2_id": "flow-2", "flow_2_variant": variants[1], **metric} for metric in metrics]
    all_records = [row for rows in flow_rows.values() for row in rows]
    row = {field: "" for field in RUN_FIELDS}; row.update({"schema_version": 1, "run_id": args.run_id, "manifest_id": json.loads((experiment / "manifest.json").read_text())["manifest_id"], "schedule_id": args.schedule_id, "session_id": "session-01", "block_id": args.block_id, "warmup": int(args.block_id == 0), "profile": args.profile, "workload": args.workload, "variant": args.variant, "status": status, "error": "" if status == "success" else "flow failure or deadline", "excluded": 0, "started_ns": common_t0, "measurement_start_ns": common_t0 + 30 * 10**9, "measurement_end_ns": common_t0 + 120 * 10**9, "ended_ns": time.monotonic_ns(), "delivered_bytes": sum(int(item["bytes"]) for item in all_records), "duration_s": 120, "goodput_bps": sum(window_goodput(rows, common_t0 + 30 * 10**9, common_t0 + 120 * 10**9) for rows in flow_rows.values()) if args.workload == "fairness" else "", "digest_ok": int(status == "success"), "records_path": f"logs/{args.run_id}/flow-*-records.csv", "queue_path": f"counters/{args.run_id}-*.json"})
    append_rows(experiment / "records.csv", RECORD_FIELDS, all_records); append_rows(experiment / "fairness.csv", FAIRNESS_FIELDS, fairness_rows); append_rows(experiment / "runs.csv", RUN_FIELDS, [row])
    return 0 if status == "success" else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    prepare = sub.add_parser("prepare-records"); prepare.add_argument("path", type=Path); prepare.add_argument("count", type=int)
    pair = sub.add_parser("run-pair"); pair.add_argument("experiment", type=Path); pair.add_argument("run_id"); pair.add_argument("schedule_id"); pair.add_argument("block_id", type=int); pair.add_argument("profile"); pair.add_argument("workload"); pair.add_argument("variant"); pair.add_argument("--port", type=int, default=9000)
    fair = sub.add_parser("run-fairness"); fair.add_argument("experiment", type=Path); fair.add_argument("run_id"); fair.add_argument("schedule_id"); fair.add_argument("block_id", type=int); fair.add_argument("profile"); fair.add_argument("workload", choices=("fairness", "fairness_step")); fair.add_argument("variant"); fair.add_argument("--port", type=int, default=9000)
    args = parser.parse_args(); root = Path(__file__).resolve().parents[2]; config = load_config(root / "tests" / "bench" / "config.json")
    if args.command == "prepare-records":
        print(prepare_file(args.path, args.count * 1024, records=True)); return 0
    required = ("RUDP_SENDER_NS", "RUDP_SHAPER_NS", "RUDP_DELAY_NS", "RUDP_RECEIVER_NS")
    if any(name not in os.environ for name in required):
        parser.error("run-pair must execute inside topology.py")
    return run_fairness(args, root, config) if args.command == "run-fairness" else run_pair(args, root, config)


if __name__ == "__main__":
    raise SystemExit(main())
