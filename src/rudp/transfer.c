#include "rudp/transfer.h"

#include <stdbool.h>
#include <string.h>

static uint64_t sender_now(const struct rudp_windowed_sender *sender)
{
    return sender->clock.now_ms(sender->clock.context);
}

static uint64_t receiver_now(const struct rudp_windowed_receiver *receiver)
{
    return receiver->clock.now_ms(receiver->clock.context);
}

static bool callbacks_valid(const struct rudp_clock *clock, const struct rudp_session_io *io)
{
    return clock != NULL && io != NULL && clock->now_ms != NULL && io->send != NULL;
}

static bool pair_valid(uint64_t client_nonce, uint64_t server_nonce,
                       const struct rudp_packet *packet)
{
    return packet->client_nonce == client_nonce && packet->server_nonce == server_nonce;
}

static enum rudp_transfer_error sender_fail(struct rudp_windowed_sender *sender,
                                            enum rudp_transfer_error error)
{
    sender->state = RUDP_TRANSFER_FAILED;
    sender->error = error;
    return error;
}

static enum rudp_transfer_error receiver_fail(struct rudp_windowed_receiver *receiver,
                                              enum rudp_transfer_error error)
{
    receiver->state = RUDP_TRANSFER_FAILED;
    receiver->error = error;
    return error;
}

static enum rudp_transfer_error sender_send(struct rudp_windowed_sender *sender,
                                            const struct rudp_packet *packet)
{
    if (sender->io.send(sender->io.context, &sender->peer, packet) != 0) {
        return sender_fail(sender, RUDP_TRANSFER_ERR_SEND);
    }
    return RUDP_TRANSFER_OK;
}

static enum rudp_transfer_error receiver_send(struct rudp_windowed_receiver *receiver,
                                              const struct rudp_packet *packet)
{
    if (receiver->io.send(receiver->io.context, &receiver->peer, packet) != 0) {
        return receiver_fail(receiver, RUDP_TRANSFER_ERR_SEND);
    }
    return RUDP_TRANSFER_OK;
}

static enum rudp_transfer_error send_data_slot(struct rudp_windowed_sender *sender,
                                               const struct rudp_send_slot *slot)
{
    const struct rudp_packet packet = {
        .type = RUDP_PACKET_DATA,
        .client_nonce = sender->client_nonce,
        .server_nonce = sender->server_nonce,
        .seq = slot->sequence,
        .data = slot->data,
        .data_length = slot->length,
    };

    return sender_send(sender, &packet);
}

static bool sender_credit_blocked(const struct rudp_windowed_sender *sender)
{
    return sender->offset < sender->source_length &&
           sender->scoreboard.next_sequence == sender->scoreboard.receive_limit;
}

static enum rudp_transfer_error sender_send_fin(struct rudp_windowed_sender *sender)
{
    const uint64_t now = sender_now(sender);
    struct rudp_packet packet = {
        .type = RUDP_PACKET_FIN,
        .client_nonce = sender->client_nonce,
        .server_nonce = sender->server_nonce,
        .seq = sender->scoreboard.next_sequence,
        .transfer_length = sender->source_length,
    };

    memcpy(packet.digest, sender->digest, sizeof(packet.digest));
    sender->state = RUDP_TRANSFER_FIN_WAIT;
    if (sender->fin_deadline_ms == 0U) {
        sender->fin_deadline_ms = now + RUDP_FIN_TIMEOUT_MS;
        sender->fin_retry_delay_ms = 1000U;
        sender->fin_retry_at_ms = now + sender->fin_retry_delay_ms;
    }
    sender->data_timer_ms = 0U;
    sender->probe_timer_ms = 0U;
    return sender_send(sender, &packet);
}

static enum rudp_transfer_error sender_fill_window(struct rudp_windowed_sender *sender)
{
    const uint64_t now = sender_now(sender);

    while (sender->offset < sender->source_length &&
           rudp_send_scoreboard_retained(&sender->scoreboard) < RUDP_WINDOW_CAPACITY &&
           rudp_send_scoreboard_flight(&sender->scoreboard) <
               rudp_fixed_cc_window(&sender->congestion) &&
           rudp_seq_in_window(sender->scoreboard.next_sequence, sender->scoreboard.cumulative_ack,
                              sender->scoreboard.receive_limit)) {
        size_t length = sender->source_length - sender->offset;
        struct rudp_send_slot *slot;

        if (length > RUDP_MAX_DATA_PAYLOAD) {
            length = RUDP_MAX_DATA_PAYLOAD;
        }
        if (!rudp_send_scoreboard_track(&sender->scoreboard, sender->scoreboard.next_sequence,
                                        sender->source + sender->offset, (uint16_t)length)) {
            return sender_fail(sender, RUDP_TRANSFER_ERR_PACKET);
        }
        slot =
            rudp_send_scoreboard_find(&sender->scoreboard, sender->scoreboard.next_sequence - 1U);
        rudp_send_scoreboard_mark_sent(slot, now);
        if (send_data_slot(sender, slot) != RUDP_TRANSFER_OK) {
            return sender->error;
        }
        sender->offset += length;
        if (sender->data_timer_ms == 0U) {
            sender->data_timer_ms = (double)now + sender->rtt.current_rto_ms;
        }
    }
    if (sender->offset == sender->source_length &&
        sender->scoreboard.cumulative_ack == sender->scoreboard.next_sequence) {
        return sender_send_fin(sender);
    }
    if (sender_credit_blocked(sender) && sender->probe_timer_ms == 0U) {
        sender->probe_delay_ms = 1000U;
        sender->probe_timer_ms = now + sender->probe_delay_ms;
    }
    return RUDP_TRANSFER_OK;
}

enum rudp_transfer_error
rudp_windowed_sender_start(struct rudp_windowed_sender *sender, const struct rudp_clock *clock,
                           const struct rudp_session_io *io, const struct rudp_peer *peer,
                           uint64_t client_nonce, uint64_t server_nonce, const uint8_t *source,
                           size_t source_length, const uint8_t digest[16], uint32_t fixed_window,
                           uint32_t initial_receive_limit)
{
    if (sender == NULL || peer == NULL || digest == NULL || !callbacks_valid(clock, io) ||
        (source_length != 0U && source == NULL) ||
        (uint64_t)source_length > RUDP_MAX_TRANSFER_LENGTH || initial_receive_limit == 0U ||
        initial_receive_limit > RUDP_WINDOW_CAPACITY) {
        return RUDP_TRANSFER_ERR_ARGUMENT;
    }
    memset(sender, 0, sizeof(*sender));
    sender->state = RUDP_TRANSFER_SENDING;
    sender->clock = *clock;
    sender->io = *io;
    sender->peer = *peer;
    sender->client_nonce = client_nonce;
    sender->server_nonce = server_nonce;
    sender->source = source;
    sender->source_length = source_length;
    memcpy(sender->digest, digest, sizeof(sender->digest));
    rudp_send_scoreboard_init(&sender->scoreboard, 0U, initial_receive_limit);
    rudp_fixed_cc_init(&sender->congestion, fixed_window);
    rudp_rtt_estimator_init(&sender->rtt);
    sender->started_ms = sender_now(sender);
    sender->progress_deadline_ms = sender->started_ms + RUDP_DATA_PROGRESS_TIMEOUT_MS;
    return sender_fill_window(sender);
}

static enum rudp_transfer_error sender_retransmit_fast(struct rudp_windowed_sender *sender)
{
    struct rudp_send_slot *slot;

    while ((slot = rudp_send_scoreboard_next_fast_retransmit(&sender->scoreboard)) != NULL) {
        if (send_data_slot(sender, slot) != RUDP_TRANSFER_OK) {
            return sender->error;
        }
        rudp_send_scoreboard_mark_retransmitted(slot);
        sender->fast_retransmits += 1U;
    }
    return RUDP_TRANSFER_OK;
}

enum rudp_transfer_error rudp_windowed_sender_receive(struct rudp_windowed_sender *sender,
                                                      const struct rudp_packet *packet)
{
    if (sender == NULL || packet == NULL) {
        return RUDP_TRANSFER_ERR_ARGUMENT;
    }
    if (!pair_valid(sender->client_nonce, sender->server_nonce, packet)) {
        return RUDP_TRANSFER_ERR_PACKET;
    }
    if (sender->state == RUDP_TRANSFER_FIN_WAIT) {
        if (packet->type == RUDP_PACKET_ACK) {
            return RUDP_TRANSFER_OK;
        }
        if (packet->type != RUDP_PACKET_FIN_ACK ||
            packet->seq != sender->scoreboard.next_sequence ||
            packet->transfer_length != sender->source_length ||
            memcmp(packet->digest, sender->digest, sizeof(sender->digest)) != 0) {
            return RUDP_TRANSFER_ERR_PACKET;
        }
        sender->state = RUDP_TRANSFER_COMPLETE;
        return RUDP_TRANSFER_OK;
    }
    if (sender->state != RUDP_TRANSFER_SENDING || packet->type != RUDP_PACKET_ACK) {
        return RUDP_TRANSFER_ERR_PACKET;
    }
    {
        struct rudp_ack_update update;
        const enum rudp_ack_result result =
            rudp_send_scoreboard_apply_ack(&sender->scoreboard, packet->ack, packet->receive_limit,
                                           packet->sacks, packet->sack_count, &update);

        if (result == RUDP_ACK_INVALID) {
            return RUDP_TRANSFER_ERR_PACKET;
        }
        if (result == RUDP_ACK_CHANGED) {
            const uint64_t now = sender_now(sender);

            if (update.cumulative_advanced || update.newly_sacked != 0U) {
                sender->progress_deadline_ms = now + RUDP_DATA_PROGRESS_TIMEOUT_MS;
            }
            if (update.credit_advanced) {
                sender->probe_timer_ms = 0U;
                sender->probe_delay_ms = 0U;
            }
            if (update.cumulative_advanced) {
                if (update.rtt_sample_suppressed) {
                    sender->suppressed_rtt_samples += 1U;
                } else if (update.rtt_sample_available) {
                    rudp_rtt_estimator_sample(&sender->rtt, (double)(now - update.rtt_sent_at_ms));
                    sender->clean_rtt_samples += 1U;
                }
                sender->data_timer_ms = rudp_send_scoreboard_flight(&sender->scoreboard) == 0U
                                            ? 0.0
                                            : (double)now + sender->rtt.current_rto_ms;
            } else if (rudp_send_scoreboard_flight(&sender->scoreboard) == 0U) {
                sender->data_timer_ms = 0.0;
            }
        }
    }
    if (sender_retransmit_fast(sender) != RUDP_TRANSFER_OK) {
        return sender->error;
    }
    return sender_fill_window(sender);
}

static struct rudp_send_slot *lowest_unsacked(struct rudp_send_scoreboard *scoreboard)
{
    uint32_t sequence;

    for (sequence = scoreboard->cumulative_ack; sequence != scoreboard->next_sequence; ++sequence) {
        struct rudp_send_slot *slot = rudp_send_scoreboard_find(scoreboard, sequence);

        if (slot != NULL && !slot->sacked) {
            return slot;
        }
    }
    return NULL;
}

enum rudp_transfer_error rudp_windowed_sender_tick(struct rudp_windowed_sender *sender)
{
    uint64_t now;

    if (sender == NULL) {
        return RUDP_TRANSFER_ERR_ARGUMENT;
    }
    if (sender->state == RUDP_TRANSFER_COMPLETE || sender->state == RUDP_TRANSFER_FAILED) {
        return sender->error;
    }
    now = sender_now(sender);
    if (now >= sender->started_ms + RUDP_TRANSFER_TIMEOUT_MS ||
        (sender->state == RUDP_TRANSFER_SENDING && now >= sender->progress_deadline_ms)) {
        return sender_fail(sender, RUDP_TRANSFER_ERR_TIMEOUT);
    }
    if (sender->state == RUDP_TRANSFER_FIN_WAIT) {
        if (now >= sender->fin_deadline_ms) {
            return sender_fail(sender, RUDP_TRANSFER_ERR_COMPLETION_UNKNOWN);
        }
        if (now >= sender->fin_retry_at_ms) {
            enum rudp_transfer_error error = sender_send_fin(sender);

            if (error != RUDP_TRANSFER_OK) {
                return error;
            }
            if (sender->fin_retry_delay_ms < 4000U) {
                sender->fin_retry_delay_ms *= 2U;
            }
            sender->fin_retry_at_ms = now + sender->fin_retry_delay_ms;
        }
        return RUDP_TRANSFER_OK;
    }
    if (sender->data_timer_ms != 0.0 && (double)now >= sender->data_timer_ms) {
        struct rudp_send_slot *slot = lowest_unsacked(&sender->scoreboard);

        if (slot != NULL) {
            if (send_data_slot(sender, slot) != RUDP_TRANSFER_OK) {
                return sender->error;
            }
            rudp_send_scoreboard_mark_retransmitted(slot);
            sender->timeout_retransmits += 1U;
            rudp_rtt_estimator_timeout(&sender->rtt);
            sender->data_timer_ms = (double)now + sender->rtt.current_rto_ms;
        } else {
            sender->data_timer_ms = 0.0;
        }
    }
    if (sender_credit_blocked(sender) && sender->probe_timer_ms != 0U &&
        now >= sender->probe_timer_ms) {
        const struct rudp_packet probe = {
            .type = RUDP_PACKET_PROBE,
            .client_nonce = sender->client_nonce,
            .server_nonce = sender->server_nonce,
        };

        if (sender_send(sender, &probe) != RUDP_TRANSFER_OK) {
            return sender->error;
        }
        if (sender->probe_delay_ms < 4000U) {
            sender->probe_delay_ms *= 2U;
        }
        sender->probe_timer_ms = now + sender->probe_delay_ms;
    }
    return RUDP_TRANSFER_OK;
}

static void receiver_make_ack(const struct rudp_windowed_receiver *receiver,
                              struct rudp_packet *packet)
{
    rudp_receive_window_make_ack(&receiver->window, receiver->client_nonce, receiver->server_nonce,
                                 packet);
}

static enum rudp_transfer_error receiver_ack(struct rudp_windowed_receiver *receiver)
{
    struct rudp_packet packet;

    receiver_make_ack(receiver, &packet);
    return receiver_send(receiver, &packet);
}

enum rudp_transfer_error rudp_windowed_receiver_start(struct rudp_windowed_receiver *receiver,
                                                      const struct rudp_clock *clock,
                                                      const struct rudp_session_io *io,
                                                      const struct rudp_peer *peer,
                                                      uint64_t client_nonce, uint64_t server_nonce,
                                                      const struct rudp_transfer_metadata *metadata,
                                                      const struct rudp_transfer_sink *sink)
{
    if (receiver == NULL || peer == NULL || metadata == NULL || sink == NULL ||
        sink->append == NULL || sink->finish == NULL || !callbacks_valid(clock, io) ||
        (metadata->length > RUDP_MAX_TRANSFER_LENGTH && metadata->length != UINT64_MAX)) {
        return RUDP_TRANSFER_ERR_ARGUMENT;
    }
    memset(receiver, 0, sizeof(*receiver));
    receiver->state = RUDP_TRANSFER_RECEIVING;
    receiver->clock = *clock;
    receiver->io = *io;
    receiver->peer = *peer;
    receiver->client_nonce = client_nonce;
    receiver->server_nonce = server_nonce;
    receiver->metadata = *metadata;
    receiver->sink = *sink;
    rudp_receive_window_init(&receiver->window, 0U);
    receiver->started_ms = receiver_now(receiver);
    receiver->progress_deadline_ms = receiver->started_ms + RUDP_DATA_PROGRESS_TIMEOUT_MS;
    return RUDP_TRANSFER_OK;
}

static bool valid_data_length(const struct rudp_windowed_receiver *receiver,
                              const struct rudp_packet *packet)
{
    uint64_t offset;
    uint64_t remaining;
    uint16_t expected;

    if (receiver->metadata.length == UINT64_MAX) {
        return packet->data_length != 0U &&
               receiver->consumed_length + packet->data_length <= RUDP_MAX_TRANSFER_LENGTH;
    }
    offset = (uint64_t)packet->seq * RUDP_MAX_DATA_PAYLOAD;
    if (offset >= receiver->metadata.length) {
        return false;
    }
    remaining = receiver->metadata.length - offset;
    expected =
        remaining > RUDP_MAX_DATA_PAYLOAD ? (uint16_t)RUDP_MAX_DATA_PAYLOAD : (uint16_t)remaining;
    return packet->data_length == expected;
}

enum rudp_transfer_error rudp_windowed_receiver_receive(struct rudp_windowed_receiver *receiver,
                                                        const struct rudp_packet *packet)
{
    uint64_t now;

    if (receiver == NULL || packet == NULL) {
        return RUDP_TRANSFER_ERR_ARGUMENT;
    }
    now = receiver_now(receiver);
    if (!pair_valid(receiver->client_nonce, receiver->server_nonce, packet)) {
        return RUDP_TRANSFER_ERR_PACKET;
    }
    if (receiver->state == RUDP_TRANSFER_LINGER) {
        if (packet->type == RUDP_PACKET_DATA || packet->type == RUDP_PACKET_PROBE) {
            return receiver_ack(receiver);
        }
        if (packet->type != RUDP_PACKET_FIN || packet->seq != receiver->window.expected ||
            packet->transfer_length != receiver->consumed_length ||
            memcmp(packet->digest, receiver->metadata.digest, sizeof(packet->digest)) != 0) {
            return RUDP_TRANSFER_ERR_PACKET;
        }
        {
            struct rudp_packet response = *packet;
            response.type = RUDP_PACKET_FIN_ACK;
            response.ack = receiver->window.expected;
            response.receive_limit = rudp_receive_window_limit(&receiver->window);
            return receiver_send(receiver, &response);
        }
    }
    if (receiver->state != RUDP_TRANSFER_RECEIVING) {
        return RUDP_TRANSFER_ERR_PACKET;
    }
    if (packet->type == RUDP_PACKET_PROBE) {
        return receiver_ack(receiver);
    }
    if (packet->type == RUDP_PACKET_DATA) {
        const uint32_t old_expected = receiver->window.expected;

        if (!valid_data_length(receiver, packet)) {
            return RUDP_TRANSFER_ERR_PACKET;
        }
        (void)rudp_receive_window_insert(&receiver->window, packet->seq, packet->data,
                                         packet->data_length);
        if (receiver->window.expected != old_expected) {
            receiver->progress_deadline_ms = now + RUDP_DATA_PROGRESS_TIMEOUT_MS;
        }
        return receiver_ack(receiver);
    }
    if (packet->type != RUDP_PACKET_FIN || packet->seq != receiver->window.expected ||
        packet->seq != receiver->window.consumed ||
        packet->transfer_length != receiver->consumed_length ||
        (receiver->metadata.length != UINT64_MAX &&
         packet->transfer_length != receiver->metadata.length) ||
        memcmp(packet->digest, receiver->metadata.digest, sizeof(packet->digest)) != 0 ||
        receiver->sink.finish(receiver->sink.context, packet->transfer_length, packet->digest) !=
            0) {
        return receiver_fail(receiver, RUDP_TRANSFER_ERR_INTEGRITY);
    }
    {
        struct rudp_packet response = *packet;
        response.type = RUDP_PACKET_FIN_ACK;
        response.ack = receiver->window.expected;
        response.receive_limit = rudp_receive_window_limit(&receiver->window);
        receiver->state = RUDP_TRANSFER_LINGER;
        receiver->linger_deadline_ms = now + RUDP_LINGER_MS;
        return receiver_send(receiver, &response);
    }
}

enum rudp_transfer_error rudp_windowed_receiver_consume(struct rudp_windowed_receiver *receiver,
                                                        size_t maximum_packets,
                                                        size_t *consumed_packets)
{
    size_t count = 0U;

    if (receiver == NULL || consumed_packets == NULL) {
        return RUDP_TRANSFER_ERR_ARGUMENT;
    }
    while (count < maximum_packets) {
        const struct rudp_receive_slot *slot =
            rudp_receive_window_next_consumable(&receiver->window);

        if (slot == NULL) {
            break;
        }
        if (receiver->sink.append(receiver->sink.context, slot->data, slot->length) != 0) {
            return receiver_fail(receiver, RUDP_TRANSFER_ERR_SINK);
        }
        receiver->consumed_length += slot->length;
        if (!rudp_receive_window_consume(&receiver->window, slot->sequence)) {
            return receiver_fail(receiver, RUDP_TRANSFER_ERR_PACKET);
        }
        count += 1U;
    }
    *consumed_packets = count;
    if (count != 0U) {
        return receiver_ack(receiver);
    }
    return RUDP_TRANSFER_OK;
}

enum rudp_transfer_error rudp_windowed_receiver_tick(struct rudp_windowed_receiver *receiver)
{
    uint64_t now;

    if (receiver == NULL) {
        return RUDP_TRANSFER_ERR_ARGUMENT;
    }
    now = receiver_now(receiver);
    if (receiver->state == RUDP_TRANSFER_LINGER && now >= receiver->linger_deadline_ms) {
        receiver->state = RUDP_TRANSFER_COMPLETE;
    } else if (receiver->state == RUDP_TRANSFER_RECEIVING &&
               (now >= receiver->started_ms + RUDP_TRANSFER_TIMEOUT_MS ||
                now >= receiver->progress_deadline_ms)) {
        return receiver_fail(receiver, RUDP_TRANSFER_ERR_TIMEOUT);
    }
    return RUDP_TRANSFER_OK;
}
