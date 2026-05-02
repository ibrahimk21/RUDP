# Design status

This document is the implementation log for the protocol design. The normative
scope and contract currently live in `PLAN.md` and `TASKS.md`; benchmark rules
live in `BENCHMARKING.md`.

## Status at repository setup

No protocol implementation has started. This initial commit establishes the
build, test, analysis, CI, and benchmark scaffolding only. Phase 0 is the next
implementation checkpoint.

## Initial decisions

- Language: C11, POSIX sockets, IPv4, Linux/WSL2 as the supported environment.
- Build: Make with strict warnings; product code will be added under `src/`.
- Test split: deterministic unit tests under `tests/unit/`, bounded live-socket
  tests under `tests/integration/`, benchmark harnesses under `tests/bench/`.
- Python is orchestration/analysis tooling, not a runtime dependency of RUDP.
- No external runtime library is required by the protocol core.

Any change to the wire contract or a normative protocol decision must update
`TASKS.md`, its tests, and this log together.
