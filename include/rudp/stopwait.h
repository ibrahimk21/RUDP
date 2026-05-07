#ifndef RUDP_STOPWAIT_H
#define RUDP_STOPWAIT_H

#include "rudp/session.h"

#include <stdbool.h>
#include <stddef.h>

#define RUDP_DATA_RETRY_MS UINT64_C(1000)
#define RUDP_DATA_PROGRESS_TIMEOUT_MS UINT64_C(120000)
#define RUDP_FIN_TIMEOUT_MS UINT64_C(30000)
#define RUDP_LINGER_MS UINT64_C(35000)
#define RUDP_TRANSFER_TIMEOUT_MS UINT64_C(1800000)

enum rudp_transfer_state {
    RUDP_TRANSFER_SENDING,
    RUDP_TRANSFER_FIN_WAIT,
    RUDP_TRANSFER_RECEIVING,
    RUDP_TRANSFER_LINGER,
    RUDP_TRANSFER_COMPLETE,
    RUDP_TRANSFER_FAILED,
};

enum rudp_transfer_error {
    RUDP_TRANSFER_OK = 0,
    RUDP_TRANSFER_ERR_ARGUMENT,
    RUDP_TRANSFER_ERR_SEND,
    RUDP_TRANSFER_ERR_SINK,
    RUDP_TRANSFER_ERR_PACKET,
    RUDP_TRANSFER_ERR_TIMEOUT,
    RUDP_TRANSFER_ERR_INTEGRITY,
    RUDP_TRANSFER_ERR_COMPLETION_UNKNOWN,
};

struct rudp_transfer_sink {
    void *context;
    int (*append)(void *context, const uint8_t *bytes, size_t length);
    int (*finish)(void *context, uint64_t length, const uint8_t digest[16]);
};

struct rudp_stopwait_sender {
    enum rudp_transfer_state state;
    enum rudp_transfer_error error;
    struct rudp_clock clock;
    struct rudp_session_io io;
    struct rudp_peer peer;
    uint64_t client_nonce;
    uint64_t server_nonce;
    const uint8_t *source;
    size_t source_length;
    size_t offset;
    uint32_t sequence;
    uint64_t started_ms;
    uint64_t progress_deadline_ms;
    uint64_t retry_at_ms;
    uint64_t fin_deadline_ms;
    uint32_t fin_retry_ms;
    uint8_t digest[16];
    struct rudp_packet outstanding;
};

struct rudp_stopwait_receiver {
    enum rudp_transfer_state state;
    enum rudp_transfer_error error;
    struct rudp_clock clock;
    struct rudp_session_io io;
    struct rudp_peer peer;
    uint64_t client_nonce;
    uint64_t server_nonce;
    struct rudp_transfer_metadata metadata;
    struct rudp_transfer_sink sink;
    uint32_t expected_sequence;
    uint64_t received_length;
    uint64_t started_ms;
    uint64_t progress_deadline_ms;
    uint64_t linger_deadline_ms;
};

enum rudp_transfer_error
rudp_stopwait_sender_start(struct rudp_stopwait_sender *sender, const struct rudp_clock *clock,
                           const struct rudp_session_io *io, const struct rudp_peer *peer,
                           uint64_t client_nonce, uint64_t server_nonce, const uint8_t *source,
                           size_t source_length, const uint8_t digest[16]);

enum rudp_transfer_error rudp_stopwait_sender_receive(struct rudp_stopwait_sender *sender,
                                                      const struct rudp_packet *packet);
enum rudp_transfer_error rudp_stopwait_sender_tick(struct rudp_stopwait_sender *sender);

enum rudp_transfer_error rudp_stopwait_receiver_start(struct rudp_stopwait_receiver *receiver,
                                                      const struct rudp_clock *clock,
                                                      const struct rudp_session_io *io,
                                                      const struct rudp_peer *peer,
                                                      uint64_t client_nonce, uint64_t server_nonce,
                                                      const struct rudp_transfer_metadata *metadata,
                                                      const struct rudp_transfer_sink *sink);

enum rudp_transfer_error rudp_stopwait_receiver_receive(struct rudp_stopwait_receiver *receiver,
                                                        const struct rudp_packet *packet);
enum rudp_transfer_error rudp_stopwait_receiver_tick(struct rudp_stopwait_receiver *receiver);

#endif
