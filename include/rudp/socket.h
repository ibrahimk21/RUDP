#ifndef RUDP_SOCKET_H
#define RUDP_SOCKET_H

#include "rudp/packet.h"
#include "rudp/session.h"

#include <stdint.h>

enum rudp_socket_error {
    RUDP_SOCKET_OK = 0,
    RUDP_SOCKET_WOULD_BLOCK,
    RUDP_SOCKET_ERR_ARGUMENT,
    RUDP_SOCKET_ERR_SYSTEM,
    RUDP_SOCKET_ERR_TRUNCATED,
    RUDP_SOCKET_ERR_PACKET,
};

struct rudp_socket {
    int fd;
    uint8_t receive_buffer[RUDP_MAX_DATAGRAM_SIZE];
};

enum rudp_socket_error rudp_socket_open(struct rudp_socket *socket, uint16_t port);
enum rudp_socket_error rudp_socket_local_port(const struct rudp_socket *socket, uint16_t *port);
void rudp_socket_close(struct rudp_socket *socket);

int rudp_socket_send_packet(void *context, const struct rudp_peer *peer,
                            const struct rudp_packet *packet);

enum rudp_socket_error rudp_socket_receive_packet(struct rudp_socket *socket,
                                                  struct rudp_peer *peer,
                                                  struct rudp_packet *packet);

#endif
