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
