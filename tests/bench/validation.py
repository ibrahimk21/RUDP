#!/usr/bin/env python3
"""Pure Phase 8 pre-collection gate calculations."""
from __future__ import annotations

import math
import re
import statistics
from collections import Counter
from typing import Any, Iterable


def validate_rtt(baseline_ms: list[float], impaired_ms: list[float], one_way_delay_ms: float) -> dict[str, Any]:
    if len(baseline_ms) < 100 or len(impaired_ms) < 100:
        return {"passed": False, "reason": "fewer than 100 successful unloaded RTT probes"}
    added = statistics.mean(impaired_ms) - statistics.mean(baseline_ms)
    target = 2 * one_way_delay_ms
    tolerance = max(5.0, target * .05)
    return {"passed": abs(added - target) <= tolerance, "mean_baseline_ms": statistics.mean(baseline_ms), "mean_impaired_ms": statistics.mean(impaired_ms), "mean_added_ms": added, "target_added_ms": target, "tolerance_ms": tolerance, "samples": len(impaired_ms)}


def loss_runs(received: list[bool]) -> list[int]:
    runs, current = [], 0
    for value in received:
        if value:
            if current:
                runs.append(current); current = 0
        else:
            current += 1
    if current:
        runs.append(current)
    return runs


def validate_random_loss(received: list[bool], expected: float) -> dict[str, Any]:
    if len(received) < 10000:
        return {"passed": False, "reason": "fewer than 10000 sequence probes"}
    realized = 1 - sum(received) / len(received)
    standard_error = math.sqrt(expected * (1 - expected) / len(received))
    tolerance = max(.005, 3 * standard_error)
    return {"passed": abs(realized - expected) <= tolerance, "sent": len(received), "lost": len(received) - sum(received), "realized_loss": realized, "expected_loss": expected, "tolerance": tolerance, "run_lengths": dict(sorted(Counter(loss_runs(received)).items()))}


def validate_leo(trials: list[list[bool]]) -> dict[str, Any]:
    if len(trials) < 10 or any(len(trial) < 10000 for trial in trials):
        return {"passed": False, "reason": "LEO requires ten 10000-packet seeds"}
    all_values = [value for trial in trials for value in trial]
    runs = [length for trial in trials for length in loss_runs(trial)]
    realized = 1 - sum(all_values) / len(all_values)
    mean_run = statistics.mean(runs) if runs else 0.0
    distribution = dict(sorted(Counter(runs).items()))
    return {"passed": abs(realized - 1 / 21) <= .01 and abs(mean_run - 5) <= 1, "seeds": len(trials), "realized_loss": realized, "expected_loss": 1 / 21, "mean_loss_run": mean_run, "expected_mean_loss_run": 5, "run_lengths": distribution}


def validate_saturation(delivered_bytes: int, duration_s: float, expected_rate_bps: float) -> dict[str, Any]:
    actual = delivered_bytes * 8 / duration_s if duration_s > 0 else 0.0
    return {"passed": duration_s == 30 and expected_rate_bps > 0 and abs(actual - expected_rate_bps) <= expected_rate_bps * .10, "delivered_rate_bps": actual, "expected_rate_bps": expected_rate_bps, "duration_s": duration_s, "tolerance_fraction": .10}


def parse_tcpdump(text: str) -> list[dict[str, Any]]:
    packets = []
    for line in text.splitlines():
        timestamp = re.match(r"(\d+\.\d+)\s", line)
        lengths = re.findall(r"length (\d+)", line)
        if timestamp and lengths:
            packets.append({"timestamp_s": float(timestamp.group(1)), "ip_bytes": int(lengths[0]), "application_bytes": int(lengths[-1]), "tcp": "Flags [" in line})
    return packets


def validate_packets(packets: Iterable[dict[str, Any]], tcp_mss: Iterable[int], requested_mss: int | None = None) -> dict[str, Any]:
    rows, mss = list(packets), [value for value in tcp_mss if value > 0]
    tcp_packets = [row for row in rows if row.get("tcp")]
    oversize = [row for row in rows if int(row["ip_bytes"]) > 1500]
    mss_ok = all(value <= 1460 for value in mss)
    if requested_mss is not None:
        # TCP_INFO reports the effective data MSS after negotiated TCP options
        # (for example timestamps), whereas TCP_MAXSEG is the requested cap.
        # Record the effective value, require that it is bounded by the cap,
        # and reject values too small to represent that negotiated adjustment.
        mss_ok = bool(mss) and all(requested_mss - 64 <= value <= requested_mss for value in mss)
    return {"passed": bool(rows) and bool(tcp_packets) and bool(mss) and not oversize and mss_ok, "packets": len(rows), "tcp_packets": len(tcp_packets), "oversize_packets": len(oversize), "maximum_ip_bytes": max((int(row["ip_bytes"]) for row in rows), default=0), "tcp_mss": mss, "requested_mss": requested_mss}


def validate_offloads(features: dict[str, dict[str, str]]) -> dict[str, Any]:
    required = ("tcp-segmentation-offload", "generic-segmentation-offload", "generic-receive-offload")
    failures = [f"{interface}:{feature}={values.get(feature, 'missing')}" for interface, values in features.items() for feature in required if values.get(feature) != "off"]
    return {"passed": bool(features) and not failures, "interfaces": len(features), "failures": failures}


def parse_tc(text: str) -> dict[str, int]:
    backlog = re.findall(r"backlog\s+(\d+)b\s+(\d+)p", text)
    counters = re.findall(r"dropped\s+(\d+),\s+overlimits\s+(\d+)\s+requeues\s+(\d+)", text)
    return {"backlog_bytes": max((int(item[0]) for item in backlog), default=0), "backlog_packets": max((int(item[1]) for item in backlog), default=0), "drops": sum(int(item[0]) for item in counters), "overlimits": sum(int(item[1]) for item in counters), "requeues": sum(int(item[2]) for item in counters)}


def validate_queues(samples: Iterable[dict[str, Any]], propagation_limit: int = 65536) -> dict[str, Any]:
    rows = list(samples)
    propagation = [row for row in rows if row.get("kind") == "propagation"]
    endpoint_drops = sum(int(row.get("endpoint_drops", 0)) for row in rows)
    too_full = [row for row in propagation if int(row.get("backlog_packets", 0)) >= propagation_limit // 10]
    return {"passed": bool(rows) and not too_full and endpoint_drops == 0, "samples": len(rows), "propagation_over_10_percent": len(too_full), "endpoint_socket_drops": endpoint_drops, "bottleneck_drops": sum(int(row.get("drops", 0)) for row in rows if row.get("kind") == "bottleneck"), "intentional_loss_is_separate": True}
