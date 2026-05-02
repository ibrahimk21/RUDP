# RUDP

RUDP is a small, understandable reliable transport over UDP in C11. It is a
laboratory project for ordered delivery, bounded failure, and congestion-control
experiments on controlled Linux network profiles. It is not authenticated,
encrypted, or intended for deployment on the public Internet.

The project is currently scaffolded and ready for Phase 0. Product
implementation intentionally has not started.

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

The repository's plans are the source of truth:

- `PLAN.md` — scope, deliverables, and completion criteria
- `TASKS.md` — protocol contract and ordered implementation checkpoints
- `BENCHMARKING.md` — experiment topology, workloads, metrics, and statistics

Do not start benchmark work before the implementation reaches the relevant
checkpoint in `TASKS.md`. Benchmark privileges are not needed for the normal
build and test commands.
