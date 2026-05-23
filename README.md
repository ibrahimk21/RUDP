# RUDP

RUDP is a small, understandable reliable transport over UDP in C11. It is a
laboratory project for ordered delivery, bounded failure, and congestion-control
experiments on controlled Linux network profiles. It is not authenticated,
encrypted, or intended for deployment on the public Internet.

Phases 0 through 8 are implemented. The
repository contains the validated packet codec and setup state machine, bounded reliable transfers, deterministic
impairment scheduling, window/flow control, SACK recovery, adaptive timers, and
a checked file-transfer CLI with deterministic correctness coverage. Phase 8
adds the frozen benchmark configuration, disposable topology, workload runners,
artifact/plot pipeline, and strict pre-collection validation gate. Privileged
calibration still has to pass on the final benchmark host before Phase 9 data.

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

The TCP reference has matching file semantics and requires an explicit
congestion-control algorithm:

```sh
./build/tcp_ref receive 9001 received.bin cubic
./build/tcp_ref send 127.0.0.1 9001 source.bin cubic
```

Replace `cubic` with `bbr` only on hosts where the kernel exposes BBR; an
unavailable request fails instead of falling back.

The unreliable UDP reference uses separate data and out-of-path control ports:

```sh
./build/udp_ref receive 9002 9003
./build/udp_ref send 127.0.0.1 9002 9003 10000
```

The optional final sender argument is a wire-rate budget in bits per second;
the default is 20,000,000. Its result reports unique bytes, duplicates,
invalid records, and missing records, and never retransmits DATA.

Reliable timed streams use the same deterministic 1024-byte record generator:

```sh
./build/rudp receive 9000 -
./build/rudp stream 127.0.0.1 9000 1000

./build/tcp_ref receive 9001 - cubic
./build/tcp_ref stream 127.0.0.1 9001 1000 cubic
```

Durations are milliseconds. Status records include common monotonic boundaries
and per-process user/system CPU time; reliable stream completion validates the
generated record sequence, final byte count, and MD5.

Each process prints one JSON status record. A receiver remains available for
the protocol's bounded FIN linger before exiting successfully.

Current local and benchmark gate status is maintained in
[docs/readiness.md](docs/readiness.md). The benchmark topology safety check is
documented in [docs/benchmark-preflight.md](docs/benchmark-preflight.md).
The full Phase 8 workflow is in
[docs/benchmark-harness.md](docs/benchmark-harness.md).
