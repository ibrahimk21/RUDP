#!/usr/bin/env python3
"""Capture CUBIC, BBR, and matched-MSS packets with CC/MSS readback."""
from __future__ import annotations

import argparse
import os
import signal
import subprocess
import time
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(); parser.add_argument("output", type=Path); parser.add_argument("--duration-ms", type=int, default=1000); parser.add_argument("--port", type=int, default=31500); args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]; args.output.mkdir(parents=True, exist_ok=True); captures = args.output / "captures"; captures.mkdir(exist_ok=True)
    sender_ns, receiver_ns, delay_ns = (os.environ.get(name) for name in ("RUDP_SENDER_NS", "RUDP_RECEIVER_NS", "RUDP_DELAY_NS"))
    if not sender_ns or not receiver_ns or not delay_ns:
        parser.error("collector must run inside topology.py")
    wrapper = root / "tests" / "bench" / "run_in_namespace.sh"; binary = root / "build" / "tcp_ref"
    result = 0
    for index, (label, cc, mss) in enumerate((("cubic", "cubic", None), ("bbr", "bbr", None), ("geo_mss_1024-cubic", "cubic", 1024), ("geo_mss_1024-bbr", "bbr", 1024))):
        port = args.port + index; pcap = captures / f"tcp-{label}.pcap"; environment = os.environ.copy()
        if mss is not None:
            environment["RUDP_TCP_MAXSEG"] = str(mss)
        capture = subprocess.Popen(["ip", "netns", "exec", delay_ns, "tcpdump", "-U", "-n", "-i", "d1", "-w", str(pcap), "tcp", "port", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        receiver_log = (args.output / f"tcp-{label}-receiver.jsonl").open("wb")
        receiver = subprocess.Popen([str(wrapper), receiver_ns, str(binary), "receive", str(port), "-", cc], stdout=receiver_log, stderr=subprocess.STDOUT, env=environment)
        time.sleep(.1)
        with (args.output / f"tcp-{label}-sender.jsonl").open("wb") as sender_log:
            sender = subprocess.run([str(wrapper), sender_ns, str(binary), "stream", "203.0.113.2", str(port), str(args.duration_ms), cc], stdout=sender_log, stderr=subprocess.STDOUT, env=environment, timeout=60, check=False)
        try:
            receiver_code = receiver.wait(timeout=60)
        except subprocess.TimeoutExpired:
            receiver.kill(); receiver_code = 124
        receiver_log.close(); capture.send_signal(signal.SIGINT); capture.wait(timeout=5)
        if sender.returncode != 0 or receiver_code != 0:
            result = 1
    return result


if __name__ == "__main__":
    raise SystemExit(main())
