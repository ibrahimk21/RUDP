#include "rudp/socket.h"
#include "rudp/transfer.h"

#include <assert.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define LIVE_TIMEOUT_MS 2000
#define LIVE_TOLERANCE_MS 30.0

static uint64_t monotonic_ms(void *context)
{
    struct timespec value;

    (void)context;
    assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
    return (uint64_t)value.tv_sec * UINT64_C(1000) + (uint64_t)value.tv_nsec / UINT64_C(1000000);
}

static void wait_delay(uint32_t delay_ms)
{
    const uint64_t deadline = monotonic_ms(NULL) + delay_ms;

    while (monotonic_ms(NULL) < deadline) {
        const uint64_t remaining = deadline - monotonic_ms(NULL);
        struct timespec request = {
            .tv_sec = (time_t)(remaining / UINT64_C(1000)),
            .tv_nsec = (long)(remaining % UINT64_C(1000)) * 1000000L,
        };

        (void)nanosleep(&request, NULL);
    }
}

static struct rudp_packet receive_bounded(struct rudp_socket *socket)
{
    struct pollfd descriptor = {.fd = socket->fd, .events = POLLIN};
    struct rudp_packet packet;
    struct rudp_peer peer;

    assert(poll(&descriptor, 1U, LIVE_TIMEOUT_MS) == 1);
    assert(rudp_socket_receive_packet(socket, &peer, &packet) == RUDP_SOCKET_OK);
    return packet;
}

static double measure_rtt(uint32_t one_way_delay_ms)
{
    static const uint8_t digest[16] = {1U};
    static const uint8_t source = 7U;
    struct rudp_socket sender_socket;
    struct rudp_socket peer_socket;
    struct rudp_peer sender_peer = {.ipv4_address = UINT32_C(0x7f000001)};
    struct rudp_peer peer = {.ipv4_address = UINT32_C(0x7f000001)};
    struct rudp_windowed_sender *sender = calloc(1U, sizeof(*sender));
    const struct rudp_clock clock = {.now_ms = monotonic_ms};
    struct rudp_session_io io;
    struct rudp_packet data;
    struct rudp_packet ack;
    double measured;

    assert(sender != NULL);
    assert(rudp_socket_open(&sender_socket, 0U) == RUDP_SOCKET_OK);
    assert(rudp_socket_open(&peer_socket, 0U) == RUDP_SOCKET_OK);
    assert(rudp_socket_local_port(&sender_socket, &sender_peer.port) == RUDP_SOCKET_OK);
    assert(rudp_socket_local_port(&peer_socket, &peer.port) == RUDP_SOCKET_OK);
    io = (struct rudp_session_io){.context = &sender_socket, .send = rudp_socket_send_packet};

    assert(rudp_windowed_sender_start(sender, &clock, &io, &peer, 11U, 22U, &source, sizeof(source),
                                      digest, 1U, RUDP_WINDOW_CAPACITY) == RUDP_TRANSFER_OK);
    data = receive_bounded(&peer_socket);
    assert(data.type == RUDP_PACKET_DATA && data.seq == 0U);
    wait_delay(one_way_delay_ms);
    ack = (struct rudp_packet){
        .type = RUDP_PACKET_ACK,
        .client_nonce = 11U,
        .server_nonce = 22U,
        .ack = 1U,
        .receive_limit = RUDP_WINDOW_CAPACITY,
    };
    assert(rudp_socket_send_packet(&peer_socket, &sender_peer, &ack) == 0);
    wait_delay(one_way_delay_ms);
    ack = receive_bounded(&sender_socket);
    assert(rudp_windowed_sender_receive(sender, &ack) == RUDP_TRANSFER_OK);
    assert(sender->clean_rtt_samples == 1U);
    measured = sender->rtt.srtt_ms;

    rudp_socket_close(&peer_socket);
    rudp_socket_close(&sender_socket);
    free(sender);
    return measured;
}

int main(void)
{
    const double baseline_ms = measure_rtt(0U);
    const double delayed_ms = measure_rtt(50U);
    const double added_ms = delayed_ms - baseline_ms;
    const double error_ms = added_ms > 100.0 ? added_ms - 100.0 : 100.0 - added_ms;

    printf("live RTT baseline=%.0f ms delayed=%.0f ms added=%.0f ms tolerance=%.0f ms\n",
           baseline_ms, delayed_ms, added_ms, LIVE_TOLERANCE_MS);
    if (error_ms > LIVE_TOLERANCE_MS) {
        fprintf(stderr,
                "measured environment failure: 50-ms scheduler delay per direction added %.0f "
                "ms RTT; expected 100+/-%.0f ms\n",
                added_ms, LIVE_TOLERANCE_MS);
        return 1;
    }
    puts("live adaptive timer delay test passed");
    return 0;
}
