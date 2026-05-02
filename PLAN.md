# Reliable Datagram Protocol: project plan

## Purpose and scope

Build a small, understandable reliable transport over UDP in C11, then measure the cost and benefit of a loss-tolerant congestion-control heuristic on simulated high-RTT links. Reliability means ordered, duplicate-free delivery or an explicit bounded failure; it cannot promise delivery through permanent loss. A negative performance result is a valid project outcome.

The supported environment is Linux (initially Ubuntu under WSL2), POSIX sockets, IPv4, one sender and one receiver per transfer, and an MTU of at least 1500 bytes. The core has no external runtime libraries; shell/Python orchestrate experiments and plots. The protocol is for trusted laboratory networks: it provides accidental-corruption detection, not authentication, encryption, protection from an on-path attacker, Internet deployment readiness, or crash-resumable transfers. MD5 is retained as an accidental-integrity check, never a security claim.

## Ownership of specifications

- This file owns scope, objectives, and completion criteria.
- [TASKS.md](TASKS.md) owns the normative protocol contract and ordered implementation checkpoints.
- [BENCHMARKING.md](BENCHMARKING.md) owns experiment configurations, workloads, metrics, and statistical rules.

Update dependent references together when changing a requirement. Keep numerical experiment parameters in BENCHMARKING.md rather than copying them into scripts and prose independently; scripts will load one versioned configuration matching that specification.

## Research questions and targets

Compare RUDP-AIMD and RUDP-Sat against explicitly selected TCP CUBIC and the available Linux BBR implementation. Record the actual kernel, algorithm, pacing, and buffer configuration; call CUBIC the host default only if verified. RUDP-AIMD is a documented simplified algorithm, not a conforming TCP Reno implementation. BBR models bottleneck bandwidth and propagation RTT; it is not simply a delay-only algorithm. A SACK-inferred loss does not establish that loss was non-congestive.

Predeclared aspirational targets, evaluated using the primary sustained workload and confidence intervals in BENCHMARKING.md:

1. Terrestrial RUDP-Sat delivered goodput at least 90% of the measured paced raw-UDP reference.
2. GEO RUDP-Sat/CUBIC goodput ratio greater than 1, and RUDP-Sat goodput at least 80% of TCP-BBR.
3. Isolated interior losses can recover without an RTO when sufficient later data arrives. Measure recovery time rather than promising a universal one-RTT bound; tail loss and small windows need timers.

Report point estimates, uncertainty, failures, CPU cost, latency, and coexistence results even when targets are missed. Missing a target does not block completion. Raw UDP is an unreliable reference, not a universal speed ceiling. Remove the former arbitrary 2,000-line promise: record core/application/test source counts separately, with the counting command, without trading correctness for size.

Mathis-style inverse-square-root loss scaling is intuition for Reno-like behavior under restrictive assumptions, not a general CUBIC prediction. Loss sweeps hold rate and RTT fixed; a zero-loss control does not enter a formula containing division by the square root of loss. Relevant primary references: [CUBIC, RFC 9438](https://www.rfc-editor.org/rfc/rfc9438.html), [TCP congestion control, RFC 5681](https://www.rfc-editor.org/rfc/rfc5681.html), [RTO estimation, RFC 6298](https://www.rfc-editor.org/rfc/rfc6298.html). Implementation differences from TCP remain explicit.

## Deliverables and layout

| Path | Responsibility |
|---|---|
| `src/rudp/` | Explicit wire codec, socket/session state, windows, SACK, timers, pluggable CC |
| `src/rudp/congestion/` | `cc.h`, `aimd.c`, `sat_tuned.c` |
| `src/common/` | File framing, digest, checked I/O, shared benchmark record helpers |
| `src/cli/` | RUDP file sender/receiver |
| `src/tcp_ref/`, `src/udp_ref/` | Verified TCP application and paced unreliable UDP reference |
| `tests/unit/`, `tests/integration/` | Fake-clock deterministic tests and bounded live socket tests |
| `tests/bench/` | Isolated network setup, configuration, validation, workload runners |
| `tools/plot_results.py` | Rebuild tables, confidence intervals, and graphs from artifacts |
| `results/<experiment-id>/` | Committed manifests, raw rows, samples, counters, and graphs |
| `docs/design.md`, `docs/report.md`, `README.md` | Design rationale, results/limitations, usage/reproduction |

Build with Make. Ignore build products and temporary transfer files, never required result artifacts. A UDP-only proxy is unnecessary: correctness uses the in-process scheduler; comparative experiments require the validated Linux network setup.

## Milestones and completion

TASKS.md Phases 0-11 are the single ordered sequence: environment; codec; session plumbing; bounded reliability; windows/SACK; adaptive timers; CLI/correctness; CC/reference tools; validated harness; baseline measurements; Sat evaluation; report.

Completion requires:

- All deterministic and bounded integration tests pass, including corruption, loss in both directions, duplication/reordering, wraparound, receiver stalls, shutdown, and failure recovery. Every successful file transfer is byte-identical and digest-verified.
- Both CC variants and both TCP baselines are measured across terrestrial, GEO, and synthetic LEO profiles; raw UDP is measured where its metrics are meaningful.
- Goodput, application latency, setup and first-byte time, controlled-loss recovery, CPU-seconds/MiB, wire/retransmission overhead, and coexistence metrics have reproducible artifacts. Exact definitions live in BENCHMARKING.md.
- A clean checkout builds/tests without benchmark privileges; a documented compatible Linux environment reproduces experiments and graphs. Benchmark unavailability is reported as incomplete experimental work, not a substitute TCP comparison.
- The report states supported claims and limitations. Synthetic profiles are not measured satellite traces, and same-host relative results can still suffer measurement bias.

Optional work begins only after these deliverables: batching/epoll optimizations, FEC, authenticated sessions, IPv6/PMTU support, alternative handshakes, or comparisons with published SCPS/PEP/KCP results. QUIC removes head-of-line blocking between independent streams, not ordered-delivery blocking within one stream; no single-file result demonstrates a QUIC advantage or replacement.
