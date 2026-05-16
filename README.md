# RUDP

RUDP is a small, understandable reliable transport over UDP in C11. It is a
laboratory project for ordered delivery, bounded failure, and congestion-control
experiments on controlled Linux network profiles. It is not authenticated,
encrypted, or intended for deployment on the public Internet.

Phases 0 through 4 are complete. The repository contains the validated packet
codec and setup state machine, bounded stop-and-wait transfers, deterministic
impairment scheduling, and a bounded windowed transfer engine with flow
control, SACK recovery, zero-credit probes, and fixed-window congestion
control. Phase 5 (adaptive timers) has not started.

## Supported development environment

- Ubuntu 24.04 under WSL2
- C11 compiler, Make, POSIX sockets
- Python 3 for experiment orchestration and plots
- Linux `ip`, `tc`, and `ethtool` for later benchmark setup

From Ubuntu/WSL:

```sh
cd /mnt/c/Users/ibrah/Documents/reliable-datagram-protocol
./tools/bootstrap_ubuntu.sh
make configure
make test
```

Benchmark privileges are not needed for the normal build and test commands.

Current local and benchmark gate status is maintained in
[docs/readiness.md](docs/readiness.md). The benchmark topology safety check is
documented in [docs/benchmark-preflight.md](docs/benchmark-preflight.md).
