# Reliable Datagram Protocol (RUDP)

[![CI](https://github.com/ibrahimk21/RUDP/actions/workflows/ci.yml/badge.svg)](https://github.com/ibrahimk21/RUDP/actions/workflows/ci.yml)

A reliable, connection-oriented transport protocol built on top of UDP in C11.
This project explores the core mechanisms behind reliable delivery—without
reimplementing TCP wholesale—and pairs the implementation with deterministic
tests, Linux network-emulation tooling, and TCP/UDP reference programs.

> **Scope:** an educational systems project for controlled Linux environments.
> It is not encrypted, authenticated, or intended for use on the public
> Internet.

## Why this project

UDP provides datagrams but leaves ordering, loss recovery, flow control, and
congestion behavior to the application. RUDP implements those missing transport
concerns explicitly, making their trade-offs visible in a compact, testable
codebase.

The project demonstrates systems-programming work across protocol design,
non-blocking sockets, timer-driven event loops, resource ownership, file I/O,
fault injection, and reproducible performance experiments.

## How it works

```text
application file / generated records
                |
                v
      RUDP transfer layer
  windows · SACK · retransmission
  adaptive RTO · AIMD · pacing
                |
                v
      session layer (handshake / close)
                |
                v
       UDP socket + event loop
```

At a high level, a sender and receiver establish a lightweight session using
nonce-validated control packets. DATA packets carry sequence numbers. The
receiver acknowledges contiguous data and selectively acknowledges out-of-order
packets; the sender maintains bounded windows, retransmits lost packets, and
adapts its retransmission timeout from measured RTT. Receiver-advertised credit
provides flow control, while AIMD congestion control and token-bucket pacing
limit how aggressively the sender injects traffic. Completion is verified with
byte counts and MD5 before the receiver atomically publishes an output file.

## Highlights

- C11 implementation with strict warnings, formatting checks, and static analysis.
- Non-blocking POSIX sockets driven by `poll` and monotonic timers.
- Ordered, bounded reliable transfer over UDP with a nonce-validated handshake.
- Sliding windows, receiver flow control, selective acknowledgements (SACK),
  fast retransmit, exponential backoff, and adaptive retransmission timers.
- AIMD congestion control and paced sending.
- Safe file-transfer boundary: source identity checks, MD5 verification, and
  atomic no-overwrite output commits.
- Deterministic unit/integration tests and a Linux `ip`/`tc` benchmark harness.
- TCP and raw-UDP reference implementations for controlled comparisons.

## Quick start

The supported development environment is Ubuntu 24.04, including Ubuntu under
WSL2. You need a C11 compiler, Make, Python 3, and POSIX sockets.

```sh
git clone https://github.com/ibrahimk21/RUDP.git
cd RUDP
./tools/bootstrap_ubuntu.sh
make configure
make test
make integration
```

`make test` runs deterministic unit tests. `make integration` runs bounded
live-socket tests. CI also checks formatting and static analysis on Ubuntu.

## Try a file transfer

Build the programs, then start the receiver before the sender. The output path
must not already exist.

```sh
make build

# Terminal 1
./build/rudp receive 9000 received.bin

# Terminal 2
./build/rudp send 127.0.0.1 9000 source.bin
```

Each process emits one JSON status record on exit. Successful transfers validate
the received length and MD5 digest before making the destination visible.

## Repository guide

| Path | Purpose |
| --- | --- |
| `include/rudp/` | Public protocol and transport interfaces. |
| `src/rudp/` | Packet codec, session state machine, windows, timers, recovery, and congestion control. |
| `src/common/` | Shared I/O, file safety, record generation, status, and MD5 support. |
| `src/cli/` | RUDP file-transfer command-line program. |
| `src/tcp_ref/`, `src/udp_ref/` | Reference implementations used by experiments. |
| `tests/` | Unit, integration, and benchmark-harness coverage. |
| `tools/` | Environment setup, test runner, and benchmark/report utilities. |
| `docs/` | Design notes and benchmark methodology. |

## Development commands

```sh
make build         # library and command-line programs
make test          # deterministic unit tests
make integration   # live loopback tests
make bench-test    # benchmark-harness tests
make format-check  # verify C formatting
make lint          # run cppcheck
make sanitize      # run unit tests with ASan and UBSan
```

## Further reading

- [Protocol design notes](docs/design.md)
- [Benchmark harness](docs/benchmark-harness.md)
- [Benchmark preflight and safety checks](docs/benchmark-preflight.md)
- [Development environment](docs/environment.md)
