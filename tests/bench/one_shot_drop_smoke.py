#!/usr/bin/env python3
"""Fast compile/attach/trigger/verify smoke test for the one-shot drop filter."""
from __future__ import annotations

import argparse
import os
import socket
import struct
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--send", action="store_true")
    args = parser.parse_args()
    record = 4
    port = 9000
    if args.send:
        payload = bytearray(24)
        payload[1] = 5
        struct.pack_into("!I", payload, 20, record)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.sendto(payload, ("203.0.113.2", port))
        return 0

    root = Path(__file__).resolve().parents[2]
    delay = os.environ["RUDP_DELAY_NS"]
    sender = os.environ["RUDP_SENDER_NS"]
    helper = root / "tests" / "bench" / "one_shot_drop.py"
    wrapper = root / "tests" / "bench" / "run_in_namespace.sh"
    subprocess.run([sys.executable, str(helper), delay, "d1", "rudp", str(port), str(record),
                    str(args.output)], check=True)
    subprocess.run([str(wrapper), sender, sys.executable, __file__, str(args.output), "--send"],
                   check=True)
    subprocess.run([sys.executable, str(helper), delay, "d1", "rudp", str(port), str(record),
                    str(args.output), "--verify"], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
