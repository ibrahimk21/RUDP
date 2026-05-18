# RUDP design notes

## Public API ownership

The codec accepts and produces `struct rudp_packet` values. A decoded DATA
payload points into the caller-owned receive buffer; it is valid only until
that buffer is reused. The session layer neither allocates packet buffers nor
retains DATA pointers in Phase 2.

`struct rudp_session` is owned by one event-loop thread. Callers initialize a
sender with `rudp_sender_start` or a receiver with `rudp_receiver_listen`,
then deliver already-decoded packets through `rudp_session_receive` and call
`rudp_session_tick` from their monotonic scheduler. The injected clock,
randomness, and send callback make that logic deterministic in tests. The
network adapter owns socket addresses and raw datagram buffers.

The send callback may return failure. A failure changes the session to
`FAILED`; callers must stop using it except to read its final error. Session
functions never block and provide no implicit queue. Later phases add bounded
DATA buffering and application backpressure.

## Setup state transitions

```text
sender:   CLOSED -> SYN_SENT -> OPEN_SENT -> ESTABLISHED -> FIN_WAIT -> CLOSED
receiver: LISTEN -> PENDING -------------> ESTABLISHED -> LINGER -> CLOSED
```

The sender generates a client nonce, sends SYN metadata, then accepts only a
matching SYN_ACK from its bound peer. It confirms both nonces with OPEN. The
receiver binds the first valid SYN peer and client nonce, generates one server
nonce, and answers duplicate SYN/OPEN packets with the original SYN_ACK or
OPEN_ACK without moving deadlines. Other peers are ignored while the receiver
is pending or active.

Setup retry intervals are 1, 2, then 4 seconds (capped at 4 seconds) within a
30-second monotonic deadline. ABORT is accepted only from the bound peer with
the active nonce pair. This phase stops at transport establishment; DATA,
closing, and linger behavior are added in later phases.

## Errors and backpressure

Malformed or state-inappropriate packets are dropped and reported to the
caller as `RUDP_SESSION_ERR_PACKET` or `RUDP_SESSION_ERR_STATE`; they do not
advance setup state. A peer mismatch returns `RUDP_SESSION_ERR_PEER`. Local
randomness, send, and timeout failures are terminal. No application bytes are
accepted in Phase 2, so there is no application backpressure yet.

## Phase 3 stop-and-wait transfer

The stop-and-wait layer is constructed only after the session handshake has
established both nonces and bound the peer. It retains one DATA packet at a
time and treats ACK `N + 1` as confirmation of DATA sequence `N`. A duplicate
or old DATA packet is not delivered again; the receiver replies with its
current cumulative ACK.

DATA retries use a fixed one-second timer in this phase. A sender or receiver
fails after 120 seconds without progress, and every transfer has an independent
30-minute deadline. FIN uses the same retained-packet model but a separate
30-second closing deadline and 1/2/4-second retry schedule. A sender that
cannot confirm FIN before that closing deadline reports
`RUDP_TRANSFER_ERR_COMPLETION_UNKNOWN`, rather than success.

The receiver writes through an injected sink. Its `finish` operation is the
output-validation boundary for this phase; Phase 6 supplies file handling and
the MD5 implementation. A verified FIN enters a 35-second linger state, where
duplicate matching FIN packets receive another FIN_ACK without committing the
sink a second time.

## Phase 4 windowed transfer

The windowed API retains a fixed 8192-slot sender ring and receiver ring. DATA
is copied into those rings, so neither decoded packet buffers nor application
write buffers need to remain valid after the corresponding call returns. The
large state objects are intended to be heap-allocated by the eventual CLI.

Receipt and consumption are deliberately separate. `expected` advances when a
contiguous DATA packet is retained, while `consumed` advances only through
`rudp_windowed_receiver_consume` after the sink accepts the bytes. Thus ACKs can
report received data without reopening credit. A successful consume sends a
window-update ACK; a credit-blocked sender uses 1/2/4-second PROBEs until one of
those updates is observed.

The sender keeps cumulatively unacknowledged payloads even after SACK and never
interprets omitted SACK blocks as reneging. Three distinct newly SACKed higher
packets arm a missing slot for fast retransmission. Once retransmitted, old
evidence cannot arm it again; three later distinct acknowledgments are needed.
The fixed congestion-control implementation limits unsacked flight during this
phase. Phase 5 replaces only the fixed DATA timer, not these ownership or flow-
control rules.

## Phase 5 adaptive DATA timer

The windowed sender begins with a 1000-ms DATA RTO. A clean advancing
cumulative ACK updates SRTT and RTTVAR with the Phase 5 estimator and restores
the computed RTO after any backoff. An ACK advance that covers a retransmitted
packet is excluded by Karn's rule and counted as a suppressed sample. SACK-only
ACKs never produce RTT samples or postpone the timer, although an all-SACKed
flight stops it.

The timer follows the lowest unsacked flight: it starts on the first original
send, restarts after an advancing cumulative ACK while unsacked DATA remains,
and doubles up to 60 seconds after an expiry. Fast retransmissions do not move
the timer. The sender exposes clean-sample, suppressed-sample, fast-retransmit,
and timeout-retransmit counters for later machine-readable reporting.

Deterministic tests compare estimator state and timer deadlines with values
calculated directly from the contract. The bounded live loopback check first
measures the uninjected scheduler/socket baseline, then adds 50 ms before DATA
delivery and 50 ms before ACK delivery. It expects 100 ms of added RTT with the
declared initial scheduler tolerance of 30 ms; an out-of-tolerance run prints
the baseline and observed values as a measured-environment failure.

## Phase 6 file boundary and CLI

`rudp send HOST PORT INPUT` snapshots a regular source file into bounded memory,
hashes it before setup, and checks its identity, length, and digest again before
reporting success. `rudp receive PORT OUTPUT` writes a unique temporary sibling,
checks every write, verifies length and MD5, flushes and closes it, and uses a
hard link followed by unlink as the Linux atomic no-replace commit. An existing
or concurrently created destination is never overwritten. Handled failures
remove the temporary file; after a process or host crash, operators may remove
leftovers matching `OUTPUT.rudp-partial.*` after confirming no receiver uses
them. Power-loss durability is outside the project scope.

Both commands use a nonblocking UDP socket, `poll`, monotonic ticks, Linux
`getrandom`, and the established session/window APIs. Exactly one JSON status
record is emitted on exit, including byte, packet, malformed-packet, RTT-sample,
and retransmission counters. These are correctness diagnostics, not benchmark
speed claims.

## Phase 7 AIMD and pacing

Production windowed senders now use RUDP-AIMD. The initial congestion window is
10 packets (or the configured smaller maximum), while the receiver window stays
the hard upper bound. Newly delivered packets grow the window exactly once,
whether learned through cumulative ACK or SACK. A fast-loss episode halves the
pre-ACK flight and suppresses further growth/reduction until the cumulative ACK
passes the highest sequence sent at entry. Every RTO restarts that boundary,
sets the window to one, and retains a floor of two for `ssthresh`.

DATA and retransmissions share a token-bucket pacer with a two-datagram burst.
Its payload rate is `cwnd * 1024 / RTT`; the initial unsampled RTT is 1000 ms.
Credit-limited pauses do not trigger idle restart. A genuinely idle sender that
resumes after at least one current RTO caps its window at 10 without changing
`ssthresh`.

The deterministic correctness matrix uses independent directional seeds. Each
finite random-loss override is an evenly distributed seeded schedule, so it is
finite and reproducible rather than a request for probabilistic eventual
success. Before the asserted run, the harness evaluates the complete schedule
once and records whether the protocol reaches success, timeout, or
completion-unknown; the asserted run must reproduce that declared outcome.
The 100-percent override must reach a bounded deadline failure without output.
Native profile loss remains a separate case. Every event needed to reproduce a
mismatch is retained and printed, including direction, original-datagram
number, time, due time, packet type/sequence, and applied actions.

## Phase 6 file boundary and CLI

`rudp send HOST PORT INPUT` snapshots a regular source file into bounded memory,
hashes it before setup, and checks its identity, length, and digest again before
reporting success. `rudp receive PORT OUTPUT` writes a unique temporary sibling,
checks every write, verifies length and MD5, flushes and closes it, and uses a
hard link followed by unlink as the Linux atomic no-replace commit. An existing
or concurrently created destination is never overwritten. Handled failures
remove the temporary file; after a process or host crash, operators may remove
leftovers matching `OUTPUT.rudp-partial.*` after confirming no receiver uses
them. Power-loss durability is outside the project scope.

Both commands use a nonblocking UDP socket, `poll`, monotonic ticks, Linux
`getrandom`, and the established session/window APIs. Exactly one JSON status
record is emitted on exit, including byte, packet, malformed-packet, RTT-sample,
and retransmission counters. These are correctness diagnostics, not benchmark
speed claims.
