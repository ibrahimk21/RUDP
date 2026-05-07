#include "rudp/stopwait.h"

#include <string.h>

static uint64_t now_ms(const struct rudp_clock *clock)
{
    return clock->now_ms(clock->context);
}

static enum rudp_transfer_error sender_fail(struct rudp_stopwait_sender *sender,
                                            enum rudp_transfer_error error)
{
    sender->state = RUDP_TRANSFER_FAILED;
    sender->error = error;
    return error;
}

static enum rudp_transfer_error receiver_fail(struct rudp_stopwait_receiver *receiver,
                                              enum rudp_transfer_error error)
{
    receiver->state = RUDP_TRANSFER_FAILED;
    receiver->error = error;
    return error;
}

static bool valid_io(const struct rudp_clock *clock, const struct rudp_session_io *io)
{
    return clock != NULL && io != NULL && clock->now_ms != NULL && io->send != NULL;
}

static bool valid_packet_pair(uint64_t client_nonce, uint64_t server_nonce,
                              const struct rudp_packet *packet)
{
    return packet->client_nonce == client_nonce && packet->server_nonce == server_nonce;
}

static enum rudp_transfer_error send_sender_packet(struct rudp_stopwait_sender *sender)
{
    if (sender->io.send(sender->io.context, &sender->peer, &sender->outstanding) != 0) {
        return sender_fail(sender, RUDP_TRANSFER_ERR_SEND);
    }
    return RUDP_TRANSFER_OK;
}

static enum rudp_transfer_error send_receiver_packet(struct rudp_stopwait_receiver *receiver,
                                                     const struct rudp_packet *packet)
{
    if (receiver->io.send(receiver->io.context, &receiver->peer, packet) != 0) {
        return receiver_fail(receiver, RUDP_TRANSFER_ERR_SEND);
    }
    return RUDP_TRANSFER_OK;
}

static void make_ack(const struct rudp_stopwait_receiver *receiver, struct rudp_packet *packet)
{
    memset(packet, 0, sizeof(*packet));
    packet->type = RUDP_PACKET_ACK;
    packet->client_nonce = receiver->client_nonce;
    packet->server_nonce = receiver->server_nonce;
    packet->ack = receiver->expected_sequence;
    packet->receive_limit = RUDP_INITIAL_RECEIVE_LIMIT;
}

static enum rudp_transfer_error sender_send_next(struct rudp_stopwait_sender *sender)
{
    const uint64_t now = now_ms(&sender->clock);

    memset(&sender->outstanding, 0, sizeof(sender->outstanding));
    sender->outstanding.client_nonce = sender->client_nonce;
    sender->outstanding.server_nonce = sender->server_nonce;
    if (sender->offset < sender->source_length) {
        size_t length = sender->source_length - sender->offset;
        if (length > RUDP_MAX_DATA_PAYLOAD) {
            length = RUDP_MAX_DATA_PAYLOAD;
        }
        sender->outstanding.type = RUDP_PACKET_DATA;
        sender->outstanding.seq = sender->sequence;
        sender->outstanding.data = sender->source + sender->offset;
        sender->outstanding.data_length = (uint16_t)length;
        sender->state = RUDP_TRANSFER_SENDING;
        sender->retry_at_ms = now + RUDP_DATA_RETRY_MS;
    } else {
        sender->outstanding.type = RUDP_PACKET_FIN;
        sender->outstanding.seq = sender->sequence;
        sender->outstanding.transfer_length = sender->source_length;
        memcpy(sender->outstanding.digest, sender->digest, sizeof(sender->digest));
        sender->state = RUDP_TRANSFER_FIN_WAIT;
        sender->fin_deadline_ms = now + RUDP_FIN_TIMEOUT_MS;
        sender->fin_retry_ms = 1000U;
        sender->retry_at_ms = now + sender->fin_retry_ms;
    }
    return send_sender_packet(sender);
}

enum rudp_transfer_error
rudp_stopwait_sender_start(struct rudp_stopwait_sender *sender, const struct rudp_clock *clock,
                           const struct rudp_session_io *io, const struct rudp_peer *peer,
                           uint64_t client_nonce, uint64_t server_nonce, const uint8_t *source,
                           size_t source_length, const uint8_t digest[16])
{
    if (sender == NULL || peer == NULL || digest == NULL || !valid_io(clock, io) ||
        (source_length != 0U && source == NULL)) {
        return RUDP_TRANSFER_ERR_ARGUMENT;
    }
    memset(sender, 0, sizeof(*sender));
    sender->clock = *clock;
    sender->io = *io;
    sender->peer = *peer;
    sender->client_nonce = client_nonce;
    sender->server_nonce = server_nonce;
    sender->source = source;
    sender->source_length = source_length;
    memcpy(sender->digest, digest, sizeof(sender->digest));
    sender->started_ms = now_ms(clock);
    sender->progress_deadline_ms = sender->started_ms + RUDP_DATA_PROGRESS_TIMEOUT_MS;
    return sender_send_next(sender);
}

enum rudp_transfer_error rudp_stopwait_sender_receive(struct rudp_stopwait_sender *sender,
                                                      const struct rudp_packet *packet)
{
    if (sender == NULL || packet == NULL) {
        return RUDP_TRANSFER_ERR_ARGUMENT;
    }
    if (sender->state == RUDP_TRANSFER_FAILED || sender->state == RUDP_TRANSFER_COMPLETE) {
        return RUDP_TRANSFER_ERR_PACKET;
    }
    if (!valid_packet_pair(sender->client_nonce, sender->server_nonce, packet)) {
        return RUDP_TRANSFER_ERR_PACKET;
    }
    if (sender->state == RUDP_TRANSFER_SENDING) {
        if (packet->type != RUDP_PACKET_ACK) {
            return RUDP_TRANSFER_ERR_PACKET;
        }
        if (packet->ack != sender->sequence + 1U) {
            return RUDP_TRANSFER_OK;
        }
        sender->offset += sender->outstanding.data_length;
        sender->sequence += 1U;
        sender->progress_deadline_ms = now_ms(&sender->clock) + RUDP_DATA_PROGRESS_TIMEOUT_MS;
        return sender_send_next(sender);
    }
    if (packet->type != RUDP_PACKET_FIN_ACK || packet->seq != sender->sequence ||
        packet->transfer_length != sender->source_length ||
        memcmp(packet->digest, sender->digest, sizeof(sender->digest)) != 0) {
        return RUDP_TRANSFER_ERR_PACKET;
    }
    sender->state = RUDP_TRANSFER_COMPLETE;
    sender->error = RUDP_TRANSFER_OK;
    return RUDP_TRANSFER_OK;
}

enum rudp_transfer_error rudp_stopwait_sender_tick(struct rudp_stopwait_sender *sender)
{
    uint64_t now;
    enum rudp_transfer_error error;

    if (sender == NULL) {
        return RUDP_TRANSFER_ERR_ARGUMENT;
    }
    now = now_ms(&sender->clock);
    if (sender->state == RUDP_TRANSFER_COMPLETE || sender->state == RUDP_TRANSFER_FAILED) {
        return RUDP_TRANSFER_OK;
    }
    if (now >= sender->started_ms + RUDP_TRANSFER_TIMEOUT_MS ||
        (sender->state == RUDP_TRANSFER_SENDING && now >= sender->progress_deadline_ms)) {
        return sender_fail(sender, RUDP_TRANSFER_ERR_TIMEOUT);
    }
    if (sender->state == RUDP_TRANSFER_FIN_WAIT && now >= sender->fin_deadline_ms) {
        return sender_fail(sender, RUDP_TRANSFER_ERR_COMPLETION_UNKNOWN);
    }
    if (now < sender->retry_at_ms) {
        return RUDP_TRANSFER_OK;
    }
    error = send_sender_packet(sender);
    if (error != RUDP_TRANSFER_OK) {
        return error;
    }
    if (sender->state == RUDP_TRANSFER_FIN_WAIT) {
        if (sender->fin_retry_ms < 4000U) {
            sender->fin_retry_ms *= 2U;
        }
        sender->retry_at_ms = now + sender->fin_retry_ms;
    } else {
        sender->retry_at_ms = now + RUDP_DATA_RETRY_MS;
    }
    return RUDP_TRANSFER_OK;
}

enum rudp_transfer_error rudp_stopwait_receiver_start(struct rudp_stopwait_receiver *receiver,
                                                      const struct rudp_clock *clock,
                                                      const struct rudp_session_io *io,
                                                      const struct rudp_peer *peer,
                                                      uint64_t client_nonce, uint64_t server_nonce,
                                                      const struct rudp_transfer_metadata *metadata,
                                                      const struct rudp_transfer_sink *sink)
{
    if (receiver == NULL || peer == NULL || metadata == NULL || sink == NULL ||
        sink->append == NULL || sink->finish == NULL || !valid_io(clock, io)) {
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
    receiver->started_ms = now_ms(clock);
    receiver->progress_deadline_ms = receiver->started_ms + RUDP_DATA_PROGRESS_TIMEOUT_MS;
    return RUDP_TRANSFER_OK;
}

enum rudp_transfer_error rudp_stopwait_receiver_receive(struct rudp_stopwait_receiver *receiver,
                                                        const struct rudp_packet *packet)
{
    struct rudp_packet response;
    const uint64_t now = now_ms(&receiver->clock);

    if (receiver == NULL || packet == NULL) {
        return RUDP_TRANSFER_ERR_ARGUMENT;
    }
    if (receiver->state == RUDP_TRANSFER_FAILED || receiver->state == RUDP_TRANSFER_COMPLETE) {
        return RUDP_TRANSFER_ERR_PACKET;
    }
    if (!valid_packet_pair(receiver->client_nonce, receiver->server_nonce, packet)) {
        return RUDP_TRANSFER_ERR_PACKET;
    }
    if (receiver->state == RUDP_TRANSFER_LINGER) {
        if (packet->type != RUDP_PACKET_FIN || packet->seq != receiver->expected_sequence ||
            packet->transfer_length != receiver->received_length ||
            memcmp(packet->digest, receiver->metadata.digest, sizeof(packet->digest)) != 0) {
            return RUDP_TRANSFER_ERR_PACKET;
        }
        response = *packet;
        response.type = RUDP_PACKET_FIN_ACK;
        response.ack = receiver->expected_sequence;
        response.receive_limit = RUDP_INITIAL_RECEIVE_LIMIT;
        return send_receiver_packet(receiver, &response);
    }
    if (packet->type == RUDP_PACKET_DATA) {
        if (packet->seq == receiver->expected_sequence) {
            if (receiver->received_length + packet->data_length > receiver->metadata.length ||
                receiver->sink.append(receiver->sink.context, packet->data, packet->data_length) !=
                    0) {
                return receiver_fail(receiver, RUDP_TRANSFER_ERR_SINK);
            }
            receiver->received_length += packet->data_length;
            receiver->expected_sequence += 1U;
            receiver->progress_deadline_ms = now + RUDP_DATA_PROGRESS_TIMEOUT_MS;
        }
        make_ack(receiver, &response);
        return send_receiver_packet(receiver, &response);
    }
    if (packet->type != RUDP_PACKET_FIN || packet->seq != receiver->expected_sequence ||
        packet->transfer_length != receiver->received_length ||
        packet->transfer_length != receiver->metadata.length ||
        memcmp(packet->digest, receiver->metadata.digest, sizeof(packet->digest)) != 0 ||
        receiver->sink.finish(receiver->sink.context, packet->transfer_length, packet->digest) !=
            0) {
        return receiver_fail(receiver, RUDP_TRANSFER_ERR_INTEGRITY);
    }
    response = *packet;
    response.type = RUDP_PACKET_FIN_ACK;
    response.ack = receiver->expected_sequence;
    response.receive_limit = RUDP_INITIAL_RECEIVE_LIMIT;
    receiver->state = RUDP_TRANSFER_LINGER;
    receiver->linger_deadline_ms = now + RUDP_LINGER_MS;
    return send_receiver_packet(receiver, &response);
}

enum rudp_transfer_error rudp_stopwait_receiver_tick(struct rudp_stopwait_receiver *receiver)
{
    uint64_t now;

    if (receiver == NULL) {
        return RUDP_TRANSFER_ERR_ARGUMENT;
    }
    now = now_ms(&receiver->clock);
    if (receiver->state == RUDP_TRANSFER_LINGER && now >= receiver->linger_deadline_ms) {
        receiver->state = RUDP_TRANSFER_COMPLETE;
        return RUDP_TRANSFER_OK;
    }
    if (receiver->state == RUDP_TRANSFER_RECEIVING &&
        (now >= receiver->started_ms + RUDP_TRANSFER_TIMEOUT_MS ||
         now >= receiver->progress_deadline_ms)) {
        return receiver_fail(receiver, RUDP_TRANSFER_ERR_TIMEOUT);
    }
    return RUDP_TRANSFER_OK;
}
