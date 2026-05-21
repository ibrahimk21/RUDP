#include "rudp/packet.h"
#include "rudp/socket.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void clobber_stack(void)
{
    volatile unsigned char bytes[4096];
    size_t index;
    for (index = 0U; index < sizeof(bytes); ++index)
        bytes[index] = (unsigned char)index;
}

int main(void)
{
    struct rudp_socket sender = {.fd = -1};
    struct rudp_socket receiver = {.fd = -1};
    struct rudp_peer destination = {.ipv4_address = UINT32_C(0x7f000001)};
    struct rudp_peer source;
    struct rudp_packet sent = {
        .type = RUDP_PACKET_DATA, .client_nonce = 1U, .server_nonce = 2U, .seq = 7U};
    struct rudp_packet received;
    uint8_t payload[1024];
    uint16_t port;
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
    size_t index;

    for (index = 0U; index < sizeof(payload); ++index)
        payload[index] = (uint8_t)(index * 17U + 9U);
    sent.data = payload;
    sent.data_length = sizeof(payload);
    assert(rudp_socket_open(&receiver, 0U) == RUDP_SOCKET_OK);
    assert(rudp_socket_local_port(&receiver, &port) == RUDP_SOCKET_OK);
    destination.port = port;
    assert(rudp_socket_open(&sender, 0U) == RUDP_SOCKET_OK);
    assert(rudp_socket_send_packet(&sender, &destination, &sent) == 0);
    for (;;) {
        enum rudp_socket_error result = rudp_socket_receive_packet(&receiver, &source, &received);
        if (result == RUDP_SOCKET_OK)
            break;
        assert(result == RUDP_SOCKET_WOULD_BLOCK);
        (void)nanosleep(&pause, NULL);
    }
    clobber_stack();
    assert(received.data_length == sizeof(payload));
    assert(memcmp(received.data, payload, sizeof(payload)) == 0);
    rudp_socket_close(&sender);
    rudp_socket_close(&receiver);
    puts("socket payload lifetime test passed");
    return 0;
}
