#!/usr/bin/env python3
"""Collect unloaded probes, offload state, counters, and capture summaries."""
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import subprocess
from pathlib import Path


def run(command: list[str]) -> str:
    return subprocess.run(command, check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout


def ping(namespace: str, address: str, count: int) -> list[dict[str, object]]:
    text = run(["ip", "netns", "exec", namespace, "ping", "-n", "-D", "-c", str(count), "-i", "0.01", "-W", "3", address])
    found = {int(match.group(1)): float(match.group(2)) for match in re.finditer(r"icmp_seq=(\d+).*?time[=<]([0-9.]+)\s*ms", text)}
    return [{"sequence": sequence, "received": int(sequence in found), "rtt_ms": found.get(sequence, "")} for sequence in range(1, count + 1)]


def sequence_probe(root: Path, source: str, destination: str, address: str, port: int, count: int) -> list[bool]:
    wrapper = root / "tests" / "bench" / "run_in_namespace.sh"; probe = root / "tests" / "bench" / "sequence_probe.py"
    receiver = subprocess.Popen([str(wrapper), destination, str(probe), "receive", str(port), str(count), "--deadline-s", "30"], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if receiver.stdout is None or receiver.stdout.readline().strip() != "READY":
        receiver.kill(); raise RuntimeError("one-way receiver did not become ready")
    subprocess.run([str(wrapper), source, str(probe), "send", address, str(port), str(count)], check=True)
    output, error = receiver.communicate(timeout=35)
    if receiver.returncode != 0:
        raise RuntimeError(error)
    observed = {int(line) for line in output.splitlines()}
    return [sequence in observed for sequence in range(count)]


def offloads(namespaces: dict[str, str]) -> dict[str, dict[str, str]]:
    interfaces = {"sender": ["s0"], "shaper": ["h0", "h1"], "delay": ["d0", "d1"], "receiver": ["r0"]}
    output = {}
    for role, devices in interfaces.items():
        for device in devices:
            text = run(["ip", "netns", "exec", namespaces[role], "ethtool", "-k", device])
            output[f"{role}:{device}"] = {match.group(1): match.group(2) for match in re.finditer(r"^([^:]+):\s+(on|off)", text, re.MULTILINE)}
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--seed", type=int, required=True)
    # Loss profiles must still retain at least 100 successful RTT observations.
    # Requesting extra probes prevents intentional loss from turning that
    # requirement into an accidental all-or-nothing gate.
    parser.add_argument("--rtt-count", type=int, default=150)
    parser.add_argument("--sequence-count", type=int, default=10000)
    args = parser.parse_args()
    names = {role: os.environ.get(f"RUDP_{role.upper()}_NS", "") for role in ("sender", "shaper", "delay", "receiver")}
    if not all(names.values()):
        parser.error("collector must run inside topology.py")
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[2]
    directions = (("forward", names["sender"], names["receiver"], "203.0.113.2", 32000), ("reverse", names["receiver"], names["sender"], "192.0.2.1", 32001))
    for direction, namespace, destination, address, port in directions:
        rtt_rows = ping(namespace, address, args.rtt_count)
        received = sequence_probe(root, namespace, destination, address, port, args.sequence_count)
        rows = [{"sequence": index + 1, "received": int(received[index]), "rtt_ms": rtt_rows[index]["rtt_ms"] if index < len(rtt_rows) else ""} for index in range(args.sequence_count)]
        with (args.output / f"{direction}.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=("profile", "seed", "direction", "sequence", "received", "rtt_ms")); writer.writeheader()
            writer.writerows({"profile": args.profile, "seed": args.seed, "direction": direction, **row} for row in rows)
    (args.output / "offloads.json").write_text(json.dumps(offloads(names), indent=2, sort_keys=True) + "\n")
    (args.output / "metadata.json").write_text(json.dumps({"profile": args.profile, "seed": args.seed, "rtt_count": args.rtt_count, "sequence_count": args.sequence_count}, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
