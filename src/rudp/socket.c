#include "rudp/socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void peer_to_address(const struct rudp_peer *peer, struct sockaddr_in *address)
{
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = htonl(peer->ipv4_address);
    address->sin_port = htons(peer->port);
}

static void address_to_peer(const struct sockaddr_in *address, struct rudp_peer *peer)
{
    peer->ipv4_address = ntohl(address->sin_addr.s_addr);
    peer->port = ntohs(address->sin_port);
}

enum rudp_socket_error rudp_socket_open(struct rudp_socket *endpoint, uint16_t port)
{
    struct sockaddr_in address;
    int flags;

    if (endpoint == NULL) {
        return RUDP_SOCKET_ERR_ARGUMENT;
    }
    endpoint->fd = -1;
    endpoint->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (endpoint->fd < 0) {
        return RUDP_SOCKET_ERR_SYSTEM;
    }
    flags = fcntl(endpoint->fd, F_GETFL);
    if (flags < 0 || fcntl(endpoint->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        rudp_socket_close(endpoint);
        return RUDP_SOCKET_ERR_SYSTEM;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(endpoint->fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        rudp_socket_close(endpoint);
        return RUDP_SOCKET_ERR_SYSTEM;
    }
    return RUDP_SOCKET_OK;
}

enum rudp_socket_error rudp_socket_local_port(const struct rudp_socket *socket, uint16_t *port)
{
    struct sockaddr_in address;
    socklen_t address_length = sizeof(address);

    if (socket == NULL || port == NULL || socket->fd < 0) {
        return RUDP_SOCKET_ERR_ARGUMENT;
    }
    if (getsockname(socket->fd, (struct sockaddr *)&address, &address_length) < 0 ||
        address_length != sizeof(address) || address.sin_family != AF_INET) {
        return RUDP_SOCKET_ERR_SYSTEM;
    }
    *port = ntohs(address.sin_port);
    return RUDP_SOCKET_OK;
}

void rudp_socket_close(struct rudp_socket *socket)
{
    if (socket != NULL && socket->fd >= 0) {
        (void)close(socket->fd);
        socket->fd = -1;
    }
}

int rudp_socket_send_packet(void *context, const struct rudp_peer *peer,
                            const struct rudp_packet *packet)
{
    struct rudp_socket *socket = context;
    struct sockaddr_in address;
    uint8_t bytes[RUDP_MAX_DATAGRAM_SIZE];
    size_t length;
    ssize_t sent;

    if (socket == NULL || peer == NULL || packet == NULL || socket->fd < 0 ||
        rudp_packet_encode(packet, bytes, sizeof(bytes), &length) != RUDP_PACKET_OK) {
        return -1;
    }
    peer_to_address(peer, &address);
    sent = sendto(socket->fd, bytes, length, 0, (const struct sockaddr *)&address, sizeof(address));
    return sent == (ssize_t)length ? 0 : -1;
}

enum rudp_socket_error rudp_socket_receive_packet(struct rudp_socket *socket,
                                                  struct rudp_peer *peer,
                                                  struct rudp_packet *packet)
{
    struct sockaddr_in address;
    struct iovec iov;
    struct msghdr message;
    uint8_t bytes[RUDP_MAX_DATAGRAM_SIZE];
    ssize_t received;

    if (socket == NULL || peer == NULL || packet == NULL || socket->fd < 0) {
        return RUDP_SOCKET_ERR_ARGUMENT;
    }
    memset(&address, 0, sizeof(address));
    memset(&message, 0, sizeof(message));
    iov.iov_base = bytes;
    iov.iov_len = sizeof(bytes);
    message.msg_name = &address;
    message.msg_namelen = sizeof(address);
    message.msg_iov = &iov;
    message.msg_iovlen = 1U;
    received = recvmsg(socket->fd, &message, 0);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return RUDP_SOCKET_WOULD_BLOCK;
        }
        return RUDP_SOCKET_ERR_SYSTEM;
    }
    if ((message.msg_flags & MSG_TRUNC) != 0 || message.msg_namelen != sizeof(struct sockaddr_in) ||
        address.sin_family != AF_INET) {
        return RUDP_SOCKET_ERR_TRUNCATED;
    }
    if (rudp_packet_decode(packet, bytes, (size_t)received) != RUDP_PACKET_OK) {
        return RUDP_SOCKET_ERR_PACKET;
    }
    address_to_peer(&address, peer);
    return RUDP_SOCKET_OK;
}
