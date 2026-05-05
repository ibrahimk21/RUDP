#include "rudp/session.h"

#include <string.h>

static bool peer_equal(const struct rudp_peer *left, const struct rudp_peer *right)
{
    return left->ipv4_address == right->ipv4_address && left->port == right->port;
}

static bool metadata_valid(const struct rudp_transfer_metadata *metadata)
{
    return metadata->length <= RUDP_MAX_TRANSFER_LENGTH || metadata->length == UINT64_MAX;
}

static bool metadata_equal(const struct rudp_transfer_metadata *left,
                           const struct rudp_packet *right)
{
    return left->length == right->transfer_length &&
           memcmp(left->digest, right->digest, sizeof(left->digest)) == 0;
}

static uint64_t now_ms(const struct rudp_session *session)
{
    return session->clock.now_ms(session->clock.context);
}

static enum rudp_session_error fail(struct rudp_session *session, enum rudp_session_error error)
{
    session->state = RUDP_SESSION_FAILED;
    session->error = error;
    return error;
}

static enum rudp_session_error send_packet(struct rudp_session *session,
                                           const struct rudp_packet *packet)
{
    if (session->io.send(session->io.context, &session->peer, packet) != 0) {
        return fail(session, RUDP_SESSION_ERR_SEND);
    }
    return RUDP_SESSION_OK;
}

static void schedule_retry(struct rudp_session *session, uint64_t now)
{
    session->next_retry_ms = now + session->retry_delay_ms;
    if (session->retry_delay_ms < 4000U) {
        session->retry_delay_ms *= 2U;
    }
}

static struct rudp_packet setup_packet(const struct rudp_session *session,
                                       enum rudp_packet_type type)
{
    struct rudp_packet packet;

    memset(&packet, 0, sizeof(packet));
    packet.type = type;
    packet.client_nonce = session->client_nonce;
    packet.server_nonce = session->server_nonce;
    packet.transfer_length = session->metadata.length;
    memcpy(packet.digest, session->metadata.digest, sizeof(packet.digest));
    return packet;
}

static enum rudp_session_error send_syn(struct rudp_session *session)
{
    struct rudp_packet packet = setup_packet(session, RUDP_PACKET_SYN);
    packet.server_nonce = 0U;
    return send_packet(session, &packet);
}

static enum rudp_session_error send_syn_ack(struct rudp_session *session)
{
    const struct rudp_packet packet = setup_packet(session, RUDP_PACKET_SYN_ACK);
    return send_packet(session, &packet);
}

static enum rudp_session_error send_open(struct rudp_session *session)
{
    const struct rudp_packet packet = setup_packet(session, RUDP_PACKET_OPEN);
    return send_packet(session, &packet);
}

static enum rudp_session_error send_open_ack(struct rudp_session *session)
{
    struct rudp_packet packet = setup_packet(session, RUDP_PACKET_OPEN_ACK);
    packet.receive_limit = RUDP_INITIAL_RECEIVE_LIMIT;
    return send_packet(session, &packet);
}

static bool valid_callbacks(const struct rudp_clock *clock, const struct rudp_random *random,
                            const struct rudp_session_io *io)
{
    return clock != NULL && random != NULL && io != NULL && clock->now_ms != NULL &&
           random->bytes != NULL && io->send != NULL;
}

static enum rudp_session_error new_nonce(struct rudp_session *session, uint64_t *nonce)
{
    if (session->random.bytes(session->random.context, (uint8_t *)nonce, sizeof(*nonce)) != 0 ||
        *nonce == 0U) {
        return fail(session, RUDP_SESSION_ERR_RANDOM);
    }
    return RUDP_SESSION_OK;
}

static void start_retries(struct rudp_session *session, uint64_t now)
{
    session->setup_deadline_ms = now + RUDP_SETUP_TIMEOUT_MS;
    session->retry_delay_ms = 1000U;
    schedule_retry(session, now);
}

enum rudp_session_error
rudp_sender_start(struct rudp_session *session, const struct rudp_clock *clock,
                  const struct rudp_random *random, const struct rudp_session_io *io,
                  const struct rudp_peer *peer, const struct rudp_transfer_metadata *metadata)
{
    enum rudp_session_error error;
    uint64_t now;

    if (session == NULL || peer == NULL || metadata == NULL ||
        !valid_callbacks(clock, random, io)) {
        return RUDP_SESSION_ERR_ARGUMENT;
    }
    if (!metadata_valid(metadata)) {
        return RUDP_SESSION_ERR_TRANSFER_SIZE;
    }

    memset(session, 0, sizeof(*session));
    session->role = RUDP_SESSION_SENDER;
    session->clock = *clock;
    session->random = *random;
    session->io = *io;
    session->peer = *peer;
    session->has_peer = true;
    session->metadata = *metadata;
    session->state = RUDP_SESSION_SYN_SENT;
    error = new_nonce(session, &session->client_nonce);
    if (error != RUDP_SESSION_OK) {
        return error;
    }
    now = now_ms(session);
    start_retries(session, now);
    return send_syn(session);
}

enum rudp_session_error rudp_receiver_listen(struct rudp_session *session,
                                             const struct rudp_clock *clock,
                                             const struct rudp_random *random,
                                             const struct rudp_session_io *io)
{
    if (session == NULL || !valid_callbacks(clock, random, io)) {
        return RUDP_SESSION_ERR_ARGUMENT;
    }
    memset(session, 0, sizeof(*session));
    session->role = RUDP_SESSION_RECEIVER;
    session->clock = *clock;
    session->random = *random;
    session->io = *io;
    session->state = RUDP_SESSION_LISTEN;
    return RUDP_SESSION_OK;
}

static enum rudp_session_error receive_sender(struct rudp_session *session,
                                              const struct rudp_packet *packet)
{
    if (session->state == RUDP_SESSION_SYN_SENT) {
        const uint64_t now = now_ms(session);

        if (packet->type != RUDP_PACKET_SYN_ACK || packet->client_nonce != session->client_nonce ||
            packet->server_nonce == 0U || !metadata_equal(&session->metadata, packet)) {
            return RUDP_SESSION_ERR_PACKET;
        }
        session->server_nonce = packet->server_nonce;
        session->state = RUDP_SESSION_OPEN_SENT;
        start_retries(session, now);
        return send_open(session);
    }
    if (session->state == RUDP_SESSION_OPEN_SENT) {
        if (packet->type == RUDP_PACKET_SYN_ACK && packet->client_nonce == session->client_nonce &&
            packet->server_nonce == session->server_nonce &&
            metadata_equal(&session->metadata, packet)) {
            return send_open(session);
        }
        if (packet->type != RUDP_PACKET_OPEN_ACK || packet->client_nonce != session->client_nonce ||
            packet->server_nonce != session->server_nonce) {
            return RUDP_SESSION_ERR_PACKET;
        }
        session->state = RUDP_SESSION_ESTABLISHED;
        session->error = RUDP_SESSION_OK;
        return RUDP_SESSION_OK;
    }
    if (session->state == RUDP_SESSION_ESTABLISHED && packet->type == RUDP_PACKET_SYN_ACK &&
        packet->client_nonce == session->client_nonce &&
        packet->server_nonce == session->server_nonce &&
        metadata_equal(&session->metadata, packet)) {
        return send_open(session);
    }
    return RUDP_SESSION_ERR_STATE;
}

static enum rudp_session_error receive_receiver(struct rudp_session *session,
                                                const struct rudp_peer *peer,
                                                const struct rudp_packet *packet)
{
    if (session->state == RUDP_SESSION_LISTEN) {
        const uint64_t now = now_ms(session);

        if (packet->type != RUDP_PACKET_SYN || packet->server_nonce != 0U ||
            !metadata_valid(&(struct rudp_transfer_metadata){
                .length = packet->transfer_length,
            })) {
            return RUDP_SESSION_ERR_PACKET;
        }
        session->peer = *peer;
        session->has_peer = true;
        session->client_nonce = packet->client_nonce;
        session->metadata.length = packet->transfer_length;
        memcpy(session->metadata.digest, packet->digest, sizeof(packet->digest));
        session->state = RUDP_SESSION_PENDING;
        if (new_nonce(session, &session->server_nonce) != RUDP_SESSION_OK) {
            return RUDP_SESSION_ERR_RANDOM;
        }
        start_retries(session, now);
        return send_syn_ack(session);
    }
    if (session->state == RUDP_SESSION_PENDING) {
        if (packet->type == RUDP_PACKET_SYN && packet->client_nonce == session->client_nonce &&
            packet->server_nonce == 0U && metadata_equal(&session->metadata, packet)) {
            return send_syn_ack(session);
        }
        if (packet->type != RUDP_PACKET_OPEN || packet->client_nonce != session->client_nonce ||
            packet->server_nonce != session->server_nonce) {
            return RUDP_SESSION_ERR_PACKET;
        }
        session->state = RUDP_SESSION_ESTABLISHED;
        session->error = RUDP_SESSION_OK;
        return send_open_ack(session);
    }
    if (session->state == RUDP_SESSION_ESTABLISHED && packet->type == RUDP_PACKET_SYN &&
        packet->client_nonce == session->client_nonce && packet->server_nonce == 0U &&
        metadata_equal(&session->metadata, packet)) {
        return send_syn_ack(session);
    }
    if (session->state == RUDP_SESSION_ESTABLISHED && packet->type == RUDP_PACKET_OPEN &&
        packet->client_nonce == session->client_nonce &&
        packet->server_nonce == session->server_nonce) {
        return send_open_ack(session);
    }
    return RUDP_SESSION_ERR_STATE;
}

enum rudp_session_error rudp_session_receive(struct rudp_session *session,
                                             const struct rudp_peer *peer,
                                             const struct rudp_packet *packet)
{
    if (session == NULL || peer == NULL || packet == NULL) {
        return RUDP_SESSION_ERR_ARGUMENT;
    }
    if (session->state == RUDP_SESSION_FAILED || session->state == RUDP_SESSION_CLOSED) {
        return RUDP_SESSION_ERR_STATE;
    }
    if (session->has_peer && !peer_equal(&session->peer, peer)) {
        return RUDP_SESSION_ERR_PEER;
    }
    if (packet->type == RUDP_PACKET_ABORT) {
        if (!session->has_peer || packet->client_nonce != session->client_nonce ||
            packet->server_nonce != session->server_nonce) {
            return RUDP_SESSION_ERR_PACKET;
        }
        return fail(session, RUDP_SESSION_ERR_ABORTED);
    }
    if (session->role == RUDP_SESSION_SENDER) {
        return receive_sender(session, packet);
    }
    return receive_receiver(session, peer, packet);
}

enum rudp_session_error rudp_session_tick(struct rudp_session *session)
{
    uint64_t now;
    enum rudp_session_error error;

    if (session == NULL) {
        return RUDP_SESSION_ERR_ARGUMENT;
    }
    if (session->state != RUDP_SESSION_SYN_SENT && session->state != RUDP_SESSION_OPEN_SENT &&
        session->state != RUDP_SESSION_PENDING) {
        return RUDP_SESSION_OK;
    }
    now = now_ms(session);
    if (now >= session->setup_deadline_ms) {
        return fail(session, RUDP_SESSION_ERR_TIMEOUT);
    }
    if (now < session->next_retry_ms) {
        return RUDP_SESSION_OK;
    }

    if (session->state == RUDP_SESSION_SYN_SENT) {
        error = send_syn(session);
    } else if (session->state == RUDP_SESSION_OPEN_SENT) {
        error = send_open(session);
    } else {
        error = send_syn_ack(session);
    }
    if (error != RUDP_SESSION_OK) {
        return error;
    }
    schedule_retry(session, now);
    return RUDP_SESSION_OK;
}

enum rudp_session_error rudp_session_abort(struct rudp_session *session, uint32_t error_code)
{
    struct rudp_packet packet;
    enum rudp_session_error error;

    if (session == NULL) {
        return RUDP_SESSION_ERR_ARGUMENT;
    }
    if (!session->has_peer || session->state != RUDP_SESSION_ESTABLISHED) {
        return RUDP_SESSION_ERR_STATE;
    }
    packet = setup_packet(session, RUDP_PACKET_ABORT);
    packet.abort_code = error_code;
    error = send_packet(session, &packet);
    if (error != RUDP_SESSION_OK) {
        return error;
    }
    return fail(session, RUDP_SESSION_ERR_ABORTED);
}

const char *rudp_session_state_string(enum rudp_session_state state)
{
    static const char *const names[] = {
        "closed",      "listen",   "syn-sent", "pending", "open-sent",
        "established", "fin-wait", "linger",   "failed",
    };

    if ((unsigned int)state >= sizeof(names) / sizeof(names[0])) {
        return "unknown";
    }
    return names[state];
}

const char *rudp_session_error_string(enum rudp_session_error error)
{
    static const char *const names[] = {
        "ok",
        "invalid argument",
        "randomness failure",
        "send failure",
        "setup timeout",
        "wrong peer",
        "invalid packet",
        "invalid state",
        "transfer too large",
        "peer aborted",
    };

    if ((unsigned int)error >= sizeof(names) / sizeof(names[0])) {
        return "unknown session error";
    }
    return names[error];
}
