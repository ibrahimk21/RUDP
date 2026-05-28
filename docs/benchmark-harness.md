# Phase 9 benchmark harness

`tests/bench/config.json` is the single machine-readable copy of the numerical
experiment configuration in `BENCHMARKING.md`. Validate it and preview the
complete randomized schedule without privileges:

```sh
make bench-test
python3 tests/bench/harness.py validate
python3 tests/bench/harness.py schedule > /tmp/rudp-schedule.csv
```

Create an immutable pilot artifact directory with:

```sh
python3 tests/bench/harness.py init pilot-YYYYMMDD --pilot
```

The directory contains the frozen configuration and repository state in
`manifest.json`, every pre-generated seed and run position in `schedule.csv`,
and versioned headers for run, record, event, queue, and counter artifacts.
Never reuse an experiment ID or remove a failed attempt. `tools/plot_results.py`
uses failed sustained runs as zero goodput, excludes only rows explicitly marked
as harness/configuration exclusions, and calculates 95% bootstrap intervals
from whole matched blocks.

On the compatible Linux benchmark host, validate required commands and both TCP
algorithms, then create the disposable topology around an orchestration command:

```sh
python3 tests/bench/topology.py --check
sudo python3 tests/bench/topology.py --profile geo \
  --forward-seed 123 --reverse-seed 456 -- ./run-one-block.sh
```

Controlled interior/tail recovery runs add `--no-loss`; the workload runner
refuses to arm its one-shot tc/eBPF classifier while profile background loss is
active. The classifier learns the TCP initial sequence number (or reads the
RUDP DATA sequence), drops only the packet covering record 4096, and records
the injection timestamp, packet span, and exact drop count. Retransmissions
bypass the already-triggered classifier.

Run a scheduled single-flow row with `workloads.py run-pair`, or a coexistence
row with `workloads.py run-fairness`. The latter releases both senders through
an explicit barrier, reverses protocol launch roles in alternating blocks,
samples both shared qdiscs once per second, and applies the 20-to-5-Mbit/s step
at 60 seconds with a 31,250-byte (50 ms) FIFO. Both commands append every
attempt—including timeouts and transport failures—to the versioned CSVs.

The topology command exports `RUDP_SENDER_NS`, `RUDP_SHAPER_NS`,
`RUDP_DELAY_NS`, and `RUDP_RECEIVER_NS`. Endpoint commands must be launched via
`tests/bench/run_in_namespace.sh "$RUDP_SENDER_NS" ...`; it drops to the
invoking sudo user (or `nobody`) while retaining privileges only in the topology
controller. An owned-resource manifest under `results/_work` enables precise
manual recovery after an uncatchable host failure. Normal exits and signals
delete only the four namespaces named in that manifest.

The compact Phase 9 collection uses one warm-up and three retained blocks. It
is descriptive rather than a high-power confirmatory study. The completed
artifact is `results/phase9-compact-20260909-r2`; regenerate its derived output
without changing the raw rows with:

```sh
python3 tools/plot_results.py results/phase9-compact-20260909-r2
python3 tools/phase9_report.py results/phase9-compact-20260909-r2
```

`REPORT.md` documents outcomes and limitations. In particular, the frozen
collector did not aggregate bidirectional wire bytes or retransmitted payload
bytes into `runs.csv`; those overhead fields are N/A, never treated as zero.
The local full packet captures are retained outside ordinary Git because they
are too large for repository hosting; the committed records table is losslessly
compressed and the manifest/counters permit independent audit.

## Mandatory pre-collection gate

On that host, collect a no-impairment baseline and every primary profile. Use
ten separately seeded topology invocations for LEO and one for the other
profiles; each invocation writes both directional 100-probe RTT and 10,000
packet one-way sequence results:

```sh
sudo tests/bench/topology.py --profile calibration_baseline -- \
  tests/bench/collect_validation.py validation/calibration_baseline/1 \
  --profile calibration_baseline --seed 1
sudo tests/bench/topology.py --profile terrestrial --forward-seed 11 \
  --reverse-seed 12 -- tests/bench/collect_validation.py \
  validation/terrestrial/1 --profile terrestrial --seed 1
```

Run `collect_saturation.py` inside a terrestrial topology, retain TCP CUBIC,
BBR, and matched-MSS smoke JSON/captures under `validation/`, convert all
before/during/after counter snapshots with `summarize_counters.py`, and run:

```sh
sudo tests/bench/cleanup_check.py validation/cleanup.json
tests/bench/failure_check.py validation/intentional-failure.json
tests/bench/preflight_gate.py validation validation/report.json
```

The final command is deliberately strict and sets `collection_permitted` only
when RTT, independent loss, ten-seed LEO loss/run length, saturation rate,
packet sizes, TCP segmentation/MSS, CUBIC/BBR readback, every interface's
TSO/GSO/GRO state, propagation occupancy, endpoint/bottleneck drops, exact
failure cleanup, and a retained intentional timeout all pass. A failed gate is
an investigation result; it is never adjusted or silently waived.
