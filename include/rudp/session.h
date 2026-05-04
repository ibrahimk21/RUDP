#ifndef RUDP_SESSION_H
#define RUDP_SESSION_H

#include "rudp/packet.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RUDP_INITIAL_RECEIVE_LIMIT 8192U
#define RUDP_MAX_TRANSFER_LENGTH (UINT64_C(16) * UINT64_C(1024) * UINT64_C(1024) * UINT64_C(1024))
#define RUDP_SETUP_TIMEOUT_MS UINT64_C(30000)

enum rudp_session_role {
    RUDP_SESSION_SENDER,
    RUDP_SESSION_RECEIVER,
};

enum rudp_session_state {
    RUDP_SESSION_CLOSED,
    RUDP_SESSION_LISTEN,
    RUDP_SESSION_SYN_SENT,
    RUDP_SESSION_PENDING,
    RUDP_SESSION_OPEN_SENT,
    RUDP_SESSION_ESTABLISHED,
    RUDP_SESSION_FIN_WAIT,
    RUDP_SESSION_LINGER,
    RUDP_SESSION_FAILED,
};

enum rudp_session_error {
    RUDP_SESSION_OK = 0,
    RUDP_SESSION_ERR_ARGUMENT,
    RUDP_SESSION_ERR_RANDOM,
    RUDP_SESSION_ERR_SEND,
    RUDP_SESSION_ERR_TIMEOUT,
    RUDP_SESSION_ERR_PEER,
    RUDP_SESSION_ERR_PACKET,
    RUDP_SESSION_ERR_STATE,
    RUDP_SESSION_ERR_TRANSFER_SIZE,
    RUDP_SESSION_ERR_ABORTED,
};

struct rudp_peer {
    uint32_t ipv4_address;
    uint16_t port;
};

struct rudp_clock {
    void *context;
    uint64_t (*now_ms)(void *context);
};

struct rudp_random {
    void *context;
    int (*bytes)(void *context, uint8_t *output, size_t output_length);
};

struct rudp_session_io {
    void *context;
    int (*send)(void *context, const struct rudp_peer *peer, const struct rudp_packet *packet);
};

struct rudp_transfer_metadata {
    uint64_t length;
    uint8_t digest[16];
};

struct rudp_session {
    enum rudp_session_role role;
    enum rudp_session_state state;
    struct rudp_clock clock;
    struct rudp_random random;
    struct rudp_session_io io;
    struct rudp_peer peer;
    bool has_peer;
    uint64_t client_nonce;
    uint64_t server_nonce;
    struct rudp_transfer_metadata metadata;
    uint64_t setup_deadline_ms;
    uint64_t next_retry_ms;
    uint32_t retry_delay_ms;
    enum rudp_session_error error;
};

enum rudp_session_error
rudp_sender_start(struct rudp_session *session, const struct rudp_clock *clock,
                  const struct rudp_random *random, const struct rudp_session_io *io,
                  const struct rudp_peer *peer, const struct rudp_transfer_metadata *metadata);

enum rudp_session_error rudp_receiver_listen(struct rudp_session *session,
                                             const struct rudp_clock *clock,
                                             const struct rudp_random *random,
                                             const struct rudp_session_io *io);

enum rudp_session_error rudp_session_receive(struct rudp_session *session,
                                             const struct rudp_peer *peer,
                                             const struct rudp_packet *packet);

enum rudp_session_error rudp_session_tick(struct rudp_session *session);

enum rudp_session_error rudp_session_abort(struct rudp_session *session, uint32_t error_code);

const char *rudp_session_state_string(enum rudp_session_state state);
const char *rudp_session_error_string(enum rudp_session_error error);

#endif
