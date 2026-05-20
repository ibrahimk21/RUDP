#!/usr/bin/env python3
"""Create exactly one disposable BENCHMARKING.md topology around a command."""
from __future__ import annotations

import argparse
import atexit
import json
import os
import shutil
import signal
import subprocess
import sys
from pathlib import Path

from harness import load_config, snapshot_counters


class Topology:
    def __init__(self, root: Path, profile_name: str, forward_seed: int, reverse_seed: int):
        self.root = root
        self.config = load_config(root / "tests" / "bench" / "config.json")
        self.profile = self.config["profiles"][profile_name]
        self.prefix = f"rudp{os.getpid():x}"[-11:]
        self.names = {role: f"{self.prefix}{suffix}" for role, suffix in (("sender", "s"), ("shaper", "h"), ("delay", "d"), ("receiver", "r"))}
        self.forward_seed = forward_seed
        self.reverse_seed = reverse_seed
        self.manifest = root / "results" / "_work" / f"{self.prefix}.topology.json"
        self.created: list[str] = []

    def run(self, *command: str, check: bool = True) -> subprocess.CompletedProcess[str]:
        return subprocess.run(command, text=True, check=check)

    def netns(self, role: str, *command: str, check: bool = True) -> subprocess.CompletedProcess[str]:
        return self.run("ip", "netns", "exec", self.names[role], *command, check=check)

    def create(self) -> None:
        self.manifest.parent.mkdir(parents=True, exist_ok=True)
        self.manifest.write_text(json.dumps({"pid": os.getpid(), "namespaces": self.names}, indent=2) + "\n")
        for role in ("sender", "shaper", "delay", "receiver"):
            self.run("ip", "netns", "add", self.names[role]); self.created.append(self.names[role]); self.netns(role, "ip", "link", "set", "lo", "up")
        links = (("sender", "s0", "shaper", "h0"), ("shaper", "h1", "delay", "d0"), ("delay", "d1", "receiver", "r0"))
        for left_role, left_if, right_role, right_if in links:
            self.run("ip", "link", "add", left_if, "type", "veth", "peer", "name", right_if)
            self.run("ip", "link", "set", left_if, "netns", self.names[left_role]); self.run("ip", "link", "set", right_if, "netns", self.names[right_role])
        addresses = (("sender", "s0", "192.0.2.1/30"), ("shaper", "h0", "192.0.2.2/30"), ("shaper", "h1", "198.51.100.1/30"), ("delay", "d0", "198.51.100.2/30"), ("delay", "d1", "203.0.113.1/30"), ("receiver", "r0", "203.0.113.2/30"))
        for role, interface, address in addresses:
            self.netns(role, "ip", "addr", "add", address, "dev", interface)
            self.netns(role, "ip", "link", "set", interface, "mtu", str(self.config["topology"]["mtu"]), "up")
            self.netns(role, "ethtool", "-K", interface, "tso", "off", "gso", "off", "gro", "off", "lro", "off", check=False)
        self.netns("sender", "ip", "route", "add", "default", "via", "192.0.2.2")
        self.netns("receiver", "ip", "route", "add", "default", "via", "203.0.113.1")
        self.netns("shaper", "ip", "route", "add", "203.0.113.0/30", "via", "198.51.100.2")
        self.netns("delay", "ip", "route", "add", "192.0.2.0/30", "via", "198.51.100.1")
        for role in ("shaper", "delay"):
            self.netns(role, "sysctl", "-q", "-w", "net.ipv4.ip_forward=1")
        for role in ("sender", "receiver"):
            self.netns(role, "sysctl", "-q", "-w", f"net.core.rmem_max={self.config['topology']['socket_buffer_bytes']}")
            self.netns(role, "sysctl", "-q", "-w", f"net.core.wmem_max={self.config['topology']['socket_buffer_bytes']}")
        self._configure_rate("shaper", "h1", self.config["topology"]["rate_bps"])
        self._configure_rate("delay", "d0", self.config["topology"]["rate_bps"])
        self._configure_netem("delay", "d1", self.forward_seed)
        self._configure_netem("shaper", "h0", self.reverse_seed)
        for role, interface in (("sender", "s0"), ("receiver", "r0")):
            self.netns(role, "tc", "qdisc", "replace", "dev", interface, "root", "fq")
        self.netns("sender", "ping", "-c", "1", "-W", "2", "203.0.113.2")

    def _configure_rate(self, role: str, interface: str, rate: int) -> None:
        topology = self.config["topology"]
        self.netns(role, "tc", "qdisc", "add", "dev", interface, "root", "handle", "1:", "htb", "default", "10")
        self.netns(role, "tc", "class", "add", "dev", interface, "parent", "1:", "classid", "1:10", "htb", "rate", f"{rate}bit", "ceil", f"{rate}bit", "burst", str(topology["htb_burst_bytes"]), "cburst", str(topology["htb_cburst_bytes"]), "quantum", str(topology["htb_quantum_bytes"]))
        self.netns(role, "tc", "qdisc", "add", "dev", interface, "parent", "1:10", "handle", "10:", "bfifo", "limit", str(topology["fifo_bytes"]))

    def _configure_netem(self, role: str, interface: str, seed: int) -> None:
        profile, topology = self.profile, self.config["topology"]
        command = ["tc", "qdisc", "add", "dev", interface, "root", "netem", "limit", str(topology["netem_limit_packets"]), "delay", f"{profile['delay_ms']}ms"]
        if "jitter_ms" in profile:
            command += [f"{profile['jitter_ms']}ms", "distribution", "uniform"]
        loss = profile["loss"]
        if loss["kind"] == "random":
            command += ["loss", "random", f"{loss['percent']}%", "seed", str(seed)]
        elif loss["kind"] == "gemodel":
            command += ["loss", "gemodel", f"{loss['p']}%", f"{loss['r']}%", f"{loss['h']}%", f"{loss['k']}%", "seed", str(seed)]
        self.netns(role, *command)

    def cleanup(self) -> None:
        for namespace in reversed(self.created):
            subprocess.run(["ip", "netns", "del", namespace], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        self.created.clear()
        self.manifest.unlink(missing_ok=True)


def check_dependencies() -> int:
    missing = [name for name in ("ip", "tc", "ethtool", "ping", "sysctl", "tcpdump", "setpriv") if shutil.which(name) is None]
    if missing:
        print("missing benchmark dependencies: " + ", ".join(missing), file=sys.stderr); return 1
    algorithms = Path("/proc/sys/net/ipv4/tcp_available_congestion_control").read_text().split()
    if not {"cubic", "bbr"}.issubset(algorithms):
        print("CUBIC and BBR must both be available", file=sys.stderr); return 1
    print("Phase 8 topology dependencies are available")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--profile", default="terrestrial")
    parser.add_argument("--forward-seed", type=int, default=1)
    parser.add_argument("--reverse-seed", type=int, default=2)
    parser.add_argument("--snapshot", type=Path)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.check:
        return check_dependencies()
    if os.geteuid() != 0 or not args.command:
        parser.error("topology creation requires root and a command after --")
    root = Path(__file__).resolve().parents[2]
    topology = Topology(root, args.profile, args.forward_seed, args.reverse_seed)
    atexit.register(topology.cleanup)
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, lambda signum, frame: sys.exit(128 + signum))
    topology.create()
    if args.snapshot:
        snapshot_counters(args.snapshot, list(topology.names.values()), "configured")
    environment = os.environ.copy(); environment.update({f"RUDP_{key.upper()}_NS": value for key, value in topology.names.items()})
    environment["RUDP_RUN_AS_UID"] = os.environ.get("SUDO_UID", "65534")
    environment["RUDP_RUN_AS_GID"] = os.environ.get("SUDO_GID", "65534")
    command = args.command[1:] if args.command and args.command[0] == "--" else args.command
    return subprocess.run(command, env=environment, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
