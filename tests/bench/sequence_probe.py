#!/usr/bin/env python3
"""Low-rate one-way sequence probe; stdout lists uniquely received IDs."""
from __future__ import annotations

import argparse
import select
import socket
import struct
import time

parser = argparse.ArgumentParser()
sub = parser.add_subparsers(dest="command", required=True)
send = sub.add_parser("send"); send.add_argument("host"); send.add_argument("port", type=int); send.add_argument("count", type=int); send.add_argument("--interval-ms", type=float, default=2)
receive = sub.add_parser("receive"); receive.add_argument("port", type=int); receive.add_argument("count", type=int); receive.add_argument("--deadline-s", type=float, default=30)
args = parser.parse_args()

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
if args.command == "send":
    sock.connect((args.host, args.port)); target = time.monotonic()
    for sequence in range(args.count):
        body = struct.pack("!Q", sequence) + bytes((sequence * 31 + index * 17) & 255 for index in range(56))
        sock.send(body); target += args.interval_ms / 1000; time.sleep(max(0, target - time.monotonic()))
else:
    sock.bind(("0.0.0.0", args.port)); sock.setblocking(False); print("READY", flush=True); received = set(); deadline = time.monotonic() + args.deadline_s; last = None
    while time.monotonic() < deadline and (last is None or time.monotonic() - last < 2):
        ready, _, _ = select.select([sock], [], [], .1)
        if not ready:
            continue
        data = sock.recv(2048); last = time.monotonic()
        if len(data) != 64:
            continue
        sequence = struct.unpack("!Q", data[:8])[0]
        expected = bytes((sequence * 31 + index * 17) & 255 for index in range(56))
        if sequence < args.count and data[8:] == expected:
            received.add(sequence)
    for sequence in sorted(received):
        print(sequence)
