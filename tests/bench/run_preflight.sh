#!/usr/bin/env bash
# Run the complete, strict Phase 9 pre-collection gate in a fresh directory.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
out=${1:?usage: run_preflight.sh OUTPUT_DIRECTORY}

collect() {
    local profile=$1 forward=$2 reverse=$3 output=$4 seed=$5
    python3 tests/bench/topology.py --profile "$profile" --forward-seed "$forward" \
        --reverse-seed "$reverse" -- python3 tests/bench/collect_validation.py \
        "$output" --profile "$profile" --seed "$seed"
}

collect calibration_baseline 1 2 "$out/calibration_baseline/1" 1
collect terrestrial 11 12 "$out/terrestrial/1" 1
collect geo 21 22 "$out/geo/1" 1
for seed in $(seq 1 10); do
    collect leo "$((100 + seed))" "$((200 + seed))" "$out/leo/$seed" "$seed"
done
python3 tests/bench/topology.py --profile terrestrial --forward-seed 301 --reverse-seed 302 -- \
    python3 tests/bench/collect_saturation.py "$out/saturation"
python3 tests/bench/topology.py --profile geo --forward-seed 303 --reverse-seed 304 -- \
    python3 tests/bench/collect_tcp_smoke.py "$out"
python3 tests/bench/topology.py --profile terrestrial --forward-seed 305 --reverse-seed 306 \
    --snapshot "$out/queue-configured.json" -- true
python3 tests/bench/summarize_counters.py "$out/queues.json" "$out/queue-configured.json"
python3 tests/bench/cleanup_check.py "$out/cleanup.json"
python3 tests/bench/failure_check.py "$out/intentional-failure.json"
python3 tests/bench/topology.py --profile terrestrial --no-loss -- \
    python3 tests/bench/one_shot_drop_smoke.py "$out/one-shot-drop.json"
python3 tests/bench/preflight_gate.py "$out" "$out/report.json"
