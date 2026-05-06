#include "rudp/session.h"
#include "rudp/socket.h"

#include <assert.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint64_t monotonic_ms(void *context)
{
    struct timespec time;

    (void)context;
    assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (uint64_t)time.tv_sec * UINT64_C(1000) + (uint64_t)time.tv_nsec / UINT64_C(1000000);
}

static int test_random(void *context, uint8_t *output, size_t output_length)
{
    uint64_t *value = context;

    assert(output_length == sizeof(*value));
    memcpy(output, value, sizeof(*value));
    *value += 1U;
    return 0;
}

static void deliver_available(struct rudp_socket *socket, struct rudp_session *session)
{
    struct rudp_packet packet;
    struct rudp_peer peer;
    enum rudp_socket_error error;

    error = rudp_socket_receive_packet(socket, &peer, &packet);
    if (error == RUDP_SOCKET_OK) {
        assert(rudp_session_receive(session, &peer, &packet) == RUDP_SESSION_OK);
    } else {
        assert(error == RUDP_SOCKET_WOULD_BLOCK);
    }
}

int main(void)
{
    struct rudp_socket sender_socket;
    struct rudp_socket receiver_socket;
    struct rudp_session sender;
    struct rudp_session receiver;
    struct rudp_peer receiver_peer = {.ipv4_address = UINT32_C(0x7f000001)};
    struct rudp_transfer_metadata metadata;
    struct rudp_clock clock = {.now_ms = monotonic_ms};
    uint64_t sender_nonce = 100U;
    uint64_t receiver_nonce = 200U;
    struct rudp_random sender_random = {.context = &sender_nonce, .bytes = test_random};
    struct rudp_random receiver_random = {.context = &receiver_nonce, .bytes = test_random};
    struct rudp_session_io sender_io = {
        .context = &sender_socket,
        .send = rudp_socket_send_packet,
    };
    struct rudp_session_io receiver_io = {
        .context = &receiver_socket,
        .send = rudp_socket_send_packet,
    };
    struct pollfd fds[2];
    uint64_t deadline;

    memset(&metadata, 0, sizeof(metadata));
    metadata.length = 12U;
    assert(rudp_socket_open(&sender_socket, 0U) == RUDP_SOCKET_OK);
    assert(rudp_socket_open(&receiver_socket, 0U) == RUDP_SOCKET_OK);
    assert(rudp_socket_local_port(&receiver_socket, &receiver_peer.port) == RUDP_SOCKET_OK);
    assert(rudp_receiver_listen(&receiver, &clock, &receiver_random, &receiver_io) ==
           RUDP_SESSION_OK);
    assert(rudp_sender_start(&sender, &clock, &sender_random, &sender_io, &receiver_peer,
                             &metadata) == RUDP_SESSION_OK);

    fds[0] = (struct pollfd){.fd = sender_socket.fd, .events = POLLIN};
    fds[1] = (struct pollfd){.fd = receiver_socket.fd, .events = POLLIN};
    deadline = monotonic_ms(NULL) + UINT64_C(2000);
    while (
        (sender.state != RUDP_SESSION_ESTABLISHED || receiver.state != RUDP_SESSION_ESTABLISHED) &&
        monotonic_ms(NULL) < deadline) {
        assert(poll(fds, 2U, 20) >= 0);
        deliver_available(&sender_socket, &sender);
        deliver_available(&receiver_socket, &receiver);
        assert(rudp_session_tick(&sender) == RUDP_SESSION_OK);
        assert(rudp_session_tick(&receiver) == RUDP_SESSION_OK);
    }
    assert(sender.state == RUDP_SESSION_ESTABLISHED);
    assert(receiver.state == RUDP_SESSION_ESTABLISHED);
    rudp_socket_close(&sender_socket);
    rudp_socket_close(&receiver_socket);
    puts("loopback setup test passed");
    return 0;
}
