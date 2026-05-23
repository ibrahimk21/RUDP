#!/usr/bin/env python3
import csv
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import harness
import topology
import validation


class HarnessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.config_path = Path(__file__).with_name("config.json")
        cls.config = harness.load_config(cls.config_path)

    def test_schedule_is_reproducible_balanced_and_seeded(self):
        first = harness.generate_schedule(self.config)
        self.assertEqual(first, harness.generate_schedule(self.config))
        self.assertEqual({row["block_id"] for row in first}, set(range(11)))
        self.assertTrue(all(row["warmup"] == (row["block_id"] == 0) for row in first))
        self.assertTrue(all(row["forward_seed"] != row["reverse_seed"] for row in first))
        grouped = {}
        for row in first:
            key = row["block_id"], row["profile"]
            grouped.setdefault(key, (row["forward_seed"], row["reverse_seed"]))
            self.assertEqual(grouped[key], (row["forward_seed"], row["reverse_seed"]))
        self.assertFalse(any(row["variant"] == "raw-udp" and row["workload"] != "sustained" for row in first))
        self.assertTrue(all("+" in row["variant"] for row in first if row["workload"].startswith("fairness")))

    def test_artifact_schema_and_immutability(self):
        with tempfile.TemporaryDirectory() as directory:
            output = harness.create_artifacts(Path(directory), "known", self.config_path)
            manifest = json.loads((output / "manifest.json").read_text())
            self.assertEqual(manifest["schema_version"], 1)
            with (output / "runs.csv").open(newline="") as stream:
                self.assertEqual(next(csv.reader(stream)), harness.RUN_FIELDS)
            with self.assertRaises(FileExistsError):
                harness.create_artifacts(Path(directory), "known", self.config_path)

    def test_timeout_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / "attempt"
            result = harness.run_command(["sh", "-c", "sleep 1"], .01, Path(str(prefix) + ".out"), Path(str(prefix) + ".err"))
            self.assertEqual(result["status"], "timeout")
            self.assertEqual(result["exit_code"], 124)

    def test_known_trace_metrics(self):
        records = [
            {"record_id": "4096", "offer_ns": "0", "delivered_ns": "2000000000", "bytes": "1024", "duplicate": "0"},
            {"record_id": "4097", "offer_ns": "0", "delivered_ns": "2500000000", "bytes": "1024", "duplicate": "0"},
            {"record_id": "4097", "offer_ns": "0", "delivered_ns": "2600000000", "bytes": "1024", "duplicate": "1"},
        ]
        self.assertEqual(harness.window_goodput(records, 1_000_000_000, 3_000_000_000), 8192.0)
        self.assertEqual(harness.setup_metrics(1_000_000, 3_000_000, 6_000_000), {"setup_ms": 2.0, "first_byte_ms": 5.0})
        recovery = harness.recovery_metrics([{"kind": "controlled_drop", "record_id": "4096", "timestamp_ns": "1500000000"}], records, 4096)
        self.assertEqual(recovery, {"drop_count": 1, "recovery_ms": 500.0, "later_data": True, "censored": False})
        fairness = harness.fairness_metrics({"flow-1": records[:1], "flow-2": records[1:2]}, [(1, 3)], 0)[0]
        self.assertEqual(fairness["jain_index"], 1.0)

    def test_failed_latency_remains_visible(self):
        metrics = harness.latency_metrics([{"offer_ns": "1000000", "delivered_ns": "2000000"}, {"offer_ns": "3000000", "delivered_ns": ""}])
        self.assertEqual(metrics["latency_samples"], 1)
        self.assertEqual(metrics["undelivered_records"], 1)
        self.assertEqual(metrics["latency_p99_ms"], 1.0)

    def test_preflight_known_distributions(self):
        baseline = [1.0] * 100
        impaired = [601.0] * 100
        self.assertTrue(validation.validate_rtt(baseline, impaired, 300)["passed"])
        random_trace = [False if index % 50 == 0 else True for index in range(10000)]
        self.assertTrue(validation.validate_random_loss(random_trace, .02)["passed"])
        leo_trial = ([False] * 5 + [True] * 100) * 96
        leo_trial = (leo_trial + [True] * 10000)[:10000]
        self.assertTrue(validation.validate_leo([leo_trial] * 10)["passed"])
        self.assertTrue(validation.validate_saturation(75_000_000, 30, 20_000_000)["passed"])

    def test_packet_offload_and_queue_gates(self):
        packets = validation.parse_tcpdump("1.000000 IP (length 1064) a > b: Flags [P.], length 1024\n1.100000 IP (length 1052) a > b: UDP, length 1024")
        self.assertTrue(validation.validate_packets(packets, [1460])["passed"])
        features = {"sender:s0": {"tcp-segmentation-offload": "off", "generic-segmentation-offload": "off", "generic-receive-offload": "off"}}
        self.assertTrue(validation.validate_offloads(features)["passed"])
        queues = [{"kind": "propagation", "backlog_packets": 10, "drops": 2}, {"kind": "bottleneck", "backlog_packets": 20, "drops": 3}]
        result = validation.validate_queues(queues)
        self.assertTrue(result["passed"])
        self.assertEqual(result["bottleneck_drops"], 3)

    def test_cleanup_deletes_only_owned_namespaces(self):
        owned = topology.Topology(Path(__file__).resolve().parents[2], "terrestrial", 1, 2)
        with tempfile.TemporaryDirectory() as directory:
            owned.manifest = Path(directory) / "owned.json"
            owned.manifest.write_text("{}")
            owned.created = ["rudp123s", "rudp123h", "rudp123d", "rudp123r"]
            with mock.patch("topology.subprocess.run") as invoked:
                owned.cleanup()
            deleted = [call.args[0][-1] for call in invoked.call_args_list]
            self.assertEqual(deleted, ["rudp123r", "rudp123d", "rudp123h", "rudp123s"])
            self.assertNotIn("unrelated", deleted)
            self.assertFalse(owned.manifest.exists())


if __name__ == "__main__":
    unittest.main()
