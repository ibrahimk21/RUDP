#!/usr/bin/env python3
"""Filesystem barrier used to release simultaneous-flow senders together."""
import argparse
import os
import time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("directory", type=Path)
parser.add_argument("participant")
parser.add_argument("participants", type=int)
parser.add_argument("command", nargs=argparse.REMAINDER)
args = parser.parse_args()
args.directory.mkdir(parents=True, exist_ok=True)
(args.directory / args.participant).touch(exist_ok=False)
deadline = time.monotonic() + 30
while len(list(args.directory.iterdir())) < args.participants:
    if time.monotonic() >= deadline:
        raise SystemExit("barrier deadline exceeded")
    time.sleep(.001)
time.sleep(.1)
command = args.command[1:] if args.command and args.command[0] == "--" else args.command
if not command:
    raise SystemExit("command required")
os.execvp(command[0], command)
