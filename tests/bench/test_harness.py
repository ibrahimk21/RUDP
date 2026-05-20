#!/usr/bin/env python3
import csv
import json
import tempfile
import unittest
from pathlib import Path

import harness


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


if __name__ == "__main__":
    unittest.main()
