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
