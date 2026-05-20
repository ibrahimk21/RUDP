# Project readiness

Status updated on 2026-09-08. These gates intentionally answer different
questions; benchmark limitations do not prevent deterministic protocol work.

The accumulated Phase 7 integration suite reports the terrestrial, GEO, and synthetic LEO
live-profile cases as `UNAVAILABLE` on this host because the isolated netem/HTB
topology cannot be created without CAP_NET_ADMIN. The 756 deterministic profile
and random-override cases still run locally. Privileged live-profile coverage
remains a Phase 8 prerequisite rather than being silently treated as passed.

| Gate | Status | Meaning |
|---|---|---|
| `correctness_ready` | **yes** | Ubuntu/WSL2 builds Phase 7 with GCC 13.3 and Make 4.3; deterministic, sanitizer, CUBIC loopback, and raw-UDP accounting tests pass. |
| `benchmark_ready` | **no** | This WSL2 instance has no CAP_NET_ADMIN for namespaces/veth/netem and its kernel exposes CUBIC but not BBR. pandas is also missing for result analysis. Phases 8-10 experiments must wait for the compatible host described in [environment.md](environment.md). |

Run the following before beginning a phase that depends on either gate:

```sh
make test
make integration
./tools/environment_report.sh
./tools/preflight_topology.sh --check
```

`preflight_topology.sh --run` is reserved for the compatible benchmark host
and requires root or CAP_NET_ADMIN. It must pass there before Phase 8 begins.
The unavailable gate is tracked rather than worked around with a UDP proxy;
that would not validate the required TCP comparisons.
