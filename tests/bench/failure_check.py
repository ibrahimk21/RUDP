#!/usr/bin/env python3
"""Create the mandatory visible, non-excluded failed harness attempt."""
import argparse
import json
import sys
from pathlib import Path

from harness import run_command

parser = argparse.ArgumentParser(); parser.add_argument("output", type=Path); args = parser.parse_args()
prefix = args.output.with_suffix("")
result = run_command([sys.executable, "-c", "import time; time.sleep(10)"], .01, Path(str(prefix) + ".stdout"), Path(str(prefix) + ".stderr"))
row = {"run_id": "intentional-timeout", "status": result["status"], "error": result["error"], "excluded": 0, "exclusion_reason": ""}
args.output.write_text(json.dumps(row, indent=2, sort_keys=True) + "\n")
raise SystemExit(0 if row["status"] == "timeout" else 1)
