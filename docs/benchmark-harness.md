# Phase 8 benchmark harness

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

The local WSL instance lacks the Phase 8 BBR/netem-seed/CAP_NET_ADMIN gate, so
configuration, schedules, metrics, schemas, failure preservation, and plots can
be tested locally, while topology calibration and measurements must run on the
compatible host described in `docs/environment.md`.
