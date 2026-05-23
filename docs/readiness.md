# Project readiness

Status updated on 2026-09-08. These gates intentionally answer different
questions; benchmark limitations do not prevent deterministic protocol work.

The accumulated Phase 8 integration suite reports the terrestrial, GEO, and synthetic LEO
live-profile cases as `UNAVAILABLE` on this host because the isolated netem/HTB
profile gate needs a kernel with BBR plus the tc/eBPF validation dependencies.
The 756 deterministic profile and random-override cases still run locally.
Privileged pre-collection calibration remains a Phase 9 prerequisite rather
than being silently treated as passed.

| Gate | Status | Meaning |
|---|---|---|
| `correctness_ready` | **yes** | Ubuntu/WSL2 builds Phase 8; deterministic, CUBIC loopback, socket-payload lifetime, timed latency, and raw-UDP accounting tests pass. |
| `benchmark_ready` | **no** | This WSL2 kernel exposes CUBIC but not BBR, and `bpftool`/pandas are missing. Phase 8's gate deliberately refuses final collection until those dependencies and all calibrated profiles pass on the compatible host described in [environment.md](environment.md). |

Run the following before beginning a phase that depends on either gate:

```sh
make test
make integration
./tools/environment_report.sh
./tools/preflight_topology.sh --check
```

`tests/bench/topology.py --check` and the complete pre-collection workflow in
[benchmark-harness.md](benchmark-harness.md) must pass before Phase 9 begins.
The unavailable gate is tracked rather than worked around with another
congestion algorithm or a UDP proxy; either would invalidate the comparisons.
