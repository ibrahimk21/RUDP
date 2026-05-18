# RUDP

RUDP is a small, understandable reliable transport over UDP in C11. It is a
laboratory project for ordered delivery, bounded failure, and congestion-control
experiments on controlled Linux network profiles. It is not authenticated,
encrypted, or intended for deployment on the public Internet.

Phases 0 through 6 are complete, with Phase 7 implementation in progress. The
repository contains the validated packet codec and setup state machine, bounded reliable transfers, deterministic
impairment scheduling, window/flow control, SACK recovery, adaptive timers, and
a checked file-transfer CLI with deterministic correctness coverage.

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

For a local file transfer, start the receiver first. The destination must not
already exist:

```sh
./build/rudp receive 9000 received.bin
./build/rudp send 127.0.0.1 9000 source.bin
```

Each process prints one JSON status record. A receiver remains available for
the protocol's bounded FIN linger before exiting successfully.

Current local and benchmark gate status is maintained in
[docs/readiness.md](docs/readiness.md). The benchmark topology safety check is
documented in [docs/benchmark-preflight.md](docs/benchmark-preflight.md).
