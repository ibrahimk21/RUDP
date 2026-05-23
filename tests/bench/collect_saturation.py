#!/usr/bin/env python3
"""Run the required 5 s settle + 30 s paced saturation calibration."""
from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import time
from pathlib import Path

from harness import load_config
from validation import parse_tcpdump


def main() -> int:
    parser = argparse.ArgumentParser(); parser.add_argument("output", type=Path); parser.add_argument("--port", type=int, default=31000); args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]; config = load_config(root / "tests" / "bench" / "config.json")
    sender_ns, receiver_ns, delay_ns = (os.environ.get(name) for name in ("RUDP_SENDER_NS", "RUDP_RECEIVER_NS", "RUDP_DELAY_NS"))
    if not sender_ns or not receiver_ns or not delay_ns:
        parser.error("collector must run inside topology.py")
    args.output.mkdir(parents=True, exist_ok=False); pcap = args.output / "saturation.pcap"
    capture = subprocess.Popen(["ip", "netns", "exec", delay_ns, "tcpdump", "-U", "-n", "-tt", "-i", "d1", "-w", str(pcap), "udp", "port", str(args.port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wrapper = root / "tests" / "bench" / "run_in_namespace.sh"; binary = root / "build" / "udp_ref"
    receiver = subprocess.Popen([str(wrapper), receiver_ns, str(binary), "receive", str(args.port), str(args.port + 1)], stdout=(args.output / "receiver.jsonl").open("wb"), stderr=subprocess.STDOUT)
    time.sleep(.1)
    duration = 35; wire = config["raw_udp"]["ip_udp_record_bytes"]; rate = config["raw_udp"]["rate_bps"]; count = rate * duration // (8 * wire)
    sender = subprocess.run([str(wrapper), sender_ns, str(binary), "send", "203.0.113.2", str(args.port), str(args.port + 1), str(count), str(rate)], stdout=(args.output / "sender.jsonl").open("wb"), stderr=subprocess.STDOUT, timeout=60, check=False)
    receiver.wait(timeout=40); capture.send_signal(signal.SIGINT); capture.wait(timeout=5)
    text = subprocess.run(["tcpdump", "-tt", "-nn", "-v", "-r", str(pcap)], check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
    packets = parse_tcpdump(text); start = min((row["timestamp_s"] for row in packets), default=0) + 5; end = start + 30
    delivered = sum(min(1024, row["application_bytes"]) for row in packets if start <= row["timestamp_s"] < end)
    expected = rate * 1024 / wire
    summary = {"duration_s": 30, "settle_s": 5, "delivered_bytes": delivered, "expected_rate_bps": expected, "sender_exit": sender.returncode, "receiver_exit": receiver.returncode, "capture": str(pcap)}
    (args.output / "saturation.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    return 0 if sender.returncode == 0 and receiver.returncode == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
