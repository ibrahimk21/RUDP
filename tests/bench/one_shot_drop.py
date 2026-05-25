#!/usr/bin/env python3
"""Compile, attach, and audit the controlled-loss tc/eBPF classifier."""
from __future__ import annotations

import argparse
import json
import platform
import struct
import subprocess
import sysconfig
from pathlib import Path


def run(command: list[str], capture: bool = False) -> str:
    result = subprocess.run(command, check=True, text=True, stdout=subprocess.PIPE if capture else None)
    return result.stdout if capture else ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("namespace")
    parser.add_argument("interface")
    parser.add_argument("protocol", choices=("rudp", "tcp"))
    parser.add_argument("port", type=int)
    parser.add_argument("record", type=int)
    parser.add_argument("output", type=Path)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    if args.verify:
        maps = json.loads(run(["bpftool", "-j", "map", "show"], True))
        candidates = [item for item in maps if item.get("name") == "rudp_drop_state"]
        if len(candidates) != 1:
            raise SystemExit("expected exactly one controlled-loss state map")
        dump = json.loads(run(["bpftool", "-j", "map", "dump", "id", str(candidates[0]["id"])], True))
        raw_value = dump[0]["value"]
        value = bytes(int(item, 16) if isinstance(item, str) else item for item in raw_value)
        tcp_base, dropped, sequence, payload, timestamp = struct.unpack("=I4xQIIQ", value)
        evidence = {"tcp_base": tcp_base, "drops": dropped, "packet_sequence": sequence, "packet_payload_bytes": payload, "injection_timestamp_ns": timestamp}
        if tcp_base:
            first_offset = max(0, sequence - tcp_base - 28)
            last_offset = max(first_offset, sequence - tcp_base - 28 + payload - 1)
            evidence["tcp_records_in_segment"] = list(range(first_offset // 1024, last_offset // 1024 + 1))
        args.output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n")
        if dropped != 1:
            raise SystemExit("controlled loss did not drop exactly one packet")
        return 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    obj = args.output.with_suffix(".bpf.o")
    architecture = {"x86_64": "x86", "aarch64": "arm64"}.get(platform.machine(), platform.machine())
    multiarch = sysconfig.get_config_var("MULTIARCH")
    include_args = []
    if multiarch and (Path("/usr/include") / multiarch).is_dir():
        include_args = ["-I", str(Path("/usr/include") / multiarch)]
    source = Path(__file__).with_name("one_shot_drop.bpf.c")
    run(["clang", "-O2", "-g", "-target", "bpf", *include_args,
         f"-D__TARGET_ARCH_{architecture}", f"-DTARGET_PORT={args.port}",
         f"-DTARGET_RECORD={args.record}", f"-DTARGET_TCP={int(args.protocol == 'tcp')}",
         "-c", str(source), "-o", str(obj)])
    run(["ip", "netns", "exec", args.namespace, "tc", "qdisc", "add", "dev", args.interface, "clsact"])
    run(["ip", "netns", "exec", args.namespace, "tc", "filter", "add", "dev", args.interface, "egress", "bpf", "direct-action", "obj", str(obj), "sec", "classifier"])
    args.output.write_text(json.dumps({"namespace": args.namespace, "interface": args.interface, "protocol": args.protocol, "port": args.port, "record": args.record, "object": str(obj)}, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
