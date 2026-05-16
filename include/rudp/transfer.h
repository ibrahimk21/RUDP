#ifndef RUDP_TRANSFER_H
#define RUDP_TRANSFER_H

#include "rudp/congestion.h"
#include "rudp/stopwait.h"
#include "rudp/window.h"

#include <stddef.h>
#include <stdint.h>

struct rudp_windowed_sender {
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
    uint8_t digest[16];
    struct rudp_send_scoreboard scoreboard;
    struct rudp_fixed_cc congestion;
    uint64_t started_ms;
    uint64_t progress_deadline_ms;
    uint64_t data_timer_ms;
    uint64_t probe_timer_ms;
    uint32_t probe_delay_ms;
    uint64_t fin_deadline_ms;
    uint64_t fin_retry_at_ms;
    uint32_t fin_retry_delay_ms;
    uint32_t fast_retransmits;
    uint32_t timeout_retransmits;
};

struct rudp_windowed_receiver {
    enum rudp_transfer_state state;
    enum rudp_transfer_error error;
    struct rudp_clock clock;
    struct rudp_session_io io;
    struct rudp_peer peer;
    uint64_t client_nonce;
    uint64_t server_nonce;
    struct rudp_transfer_metadata metadata;
    struct rudp_transfer_sink sink;
    struct rudp_receive_window window;
    uint64_t consumed_length;
    uint64_t started_ms;
    uint64_t progress_deadline_ms;
    uint64_t linger_deadline_ms;
};

enum rudp_transfer_error
rudp_windowed_sender_start(struct rudp_windowed_sender *sender, const struct rudp_clock *clock,
                           const struct rudp_session_io *io, const struct rudp_peer *peer,
                           uint64_t client_nonce, uint64_t server_nonce, const uint8_t *source,
                           size_t source_length, const uint8_t digest[16], uint32_t fixed_window,
                           uint32_t initial_receive_limit);
enum rudp_transfer_error rudp_windowed_sender_receive(struct rudp_windowed_sender *sender,
                                                       const struct rudp_packet *packet);
enum rudp_transfer_error rudp_windowed_sender_tick(struct rudp_windowed_sender *sender);

enum rudp_transfer_error
rudp_windowed_receiver_start(struct rudp_windowed_receiver *receiver, const struct rudp_clock *clock,
                             const struct rudp_session_io *io, const struct rudp_peer *peer,
                             uint64_t client_nonce, uint64_t server_nonce,
                             const struct rudp_transfer_metadata *metadata,
                             const struct rudp_transfer_sink *sink);
enum rudp_transfer_error rudp_windowed_receiver_receive(struct rudp_windowed_receiver *receiver,
                                                         const struct rudp_packet *packet);
enum rudp_transfer_error rudp_windowed_receiver_consume(struct rudp_windowed_receiver *receiver,
                                                         size_t maximum_packets,
                                                         size_t *consumed_packets);
enum rudp_transfer_error rudp_windowed_receiver_tick(struct rudp_windowed_receiver *receiver);

#endif
