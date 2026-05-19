#include "rudp/benchmark.h"
#include "rudp/file.h"
#include "rudp/session.h"
#include "rudp/socket.h"
#include "rudp/transfer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

struct cli_context {
    struct rudp_socket socket;
    struct rudp_session session;
    struct rudp_windowed_sender *sender;
    struct rudp_windowed_receiver *receiver;
    struct rudp_source_file source;
    struct rudp_output_file output;
    struct rudp_peer peer;
    bool sending;
    bool transfer_started;
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t malformed_packets;
};

static uint64_t monotonic_ms(void *unused)
{
    struct timespec value;

    (void)unused;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0U;
    }
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static int secure_random(void *unused, uint8_t *output, size_t length)
{
    size_t offset = 0U;

    (void)unused;
    while (offset != length) {
        ssize_t count = getrandom(output + offset, length - offset, 0);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return -1;
        }
        offset += (size_t)count;
    }
    return 0;
}

static int counted_send(void *context, const struct rudp_peer *peer,
                        const struct rudp_packet *packet)
{
    struct cli_context *cli = context;
    int result = rudp_socket_send_packet(&cli->socket, peer, packet);

    if (result == 0) {
        cli->packets_sent += 1U;
    }
    return result;
}

static bool peer_equal(const struct rudp_peer *left, const struct rudp_peer *right)
{
    return left->ipv4_address == right->ipv4_address && left->port == right->port;
}

static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0U || value > UINT16_MAX) {
        errno = EINVAL;
        return -1;
    }
    *port = (uint16_t)value;
    return 0;
}

static int resolve_peer(const char *host, uint16_t port, struct rudp_peer *peer)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *current;
    char service[6];
    int result = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    (void)snprintf(service, sizeof(service), "%u", (unsigned int)port);
    if (getaddrinfo(host, service, &hints, &addresses) != 0) {
        errno = EINVAL;
        return -1;
    }
    for (current = addresses; current != NULL; current = current->ai_next) {
        const struct sockaddr_in *address = (const struct sockaddr_in *)current->ai_addr;

        if (current->ai_addrlen == sizeof(*address)) {
            peer->ipv4_address = ntohl(address->sin_addr.s_addr);
            peer->port = ntohs(address->sin_port);
            result = 0;
            break;
        }
    }
    freeaddrinfo(addresses);
    return result;
}

static const char *transfer_error_string(enum rudp_transfer_error error)
{
    static const char *const names[] = {"ok",        "argument", "send",
                                        "sink",      "packet",   "timeout",
                                        "integrity", "limit",    "completion_unknown"};

    return (unsigned int)error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown";
}

static void print_status(const struct cli_context *cli, bool success, const char *stage,
                         const char *error)
{
    const struct rudp_benchmark_record record = {
        .tool = "rudp",
        .role = cli->sending ? "sender" : "receiver",
        .status = success ? "success" : "failure",
        .stage = stage,
        .error = error,
        .cc_requested = "aimd",
        .cc_actual = "aimd",
        .bytes = cli->sending ? cli->source.length : cli->output.length,
        .packets_sent = cli->packets_sent,
        .packets_received = cli->packets_received,
        .malformed_packets = cli->malformed_packets,
        .fast_retransmits = cli->sender == NULL ? 0U : cli->sender->fast_retransmits,
        .timeout_retransmits = cli->sender == NULL ? 0U : cli->sender->timeout_retransmits,
        .clean_rtt_samples = cli->sender == NULL ? 0U : cli->sender->clean_rtt_samples,
        .suppressed_rtt_samples = cli->sender == NULL ? 0U : cli->sender->suppressed_rtt_samples,
    };

    (void)rudp_benchmark_record_write(stdout, &record);
}

static int start_transfer(struct cli_context *cli, const struct rudp_clock *clock,
                          const struct rudp_session_io *io)
{
    if (cli->transfer_started || cli->session.state != RUDP_SESSION_ESTABLISHED) {
        return 0;
    }
    if (cli->sending) {
        cli->sender = calloc(1U, sizeof(*cli->sender));
        if (cli->sender == NULL ||
            rudp_windowed_sender_start(cli->sender, clock, io, &cli->session.peer,
                                       cli->session.client_nonce, cli->session.server_nonce,
                                       cli->source.bytes, cli->source.length, cli->source.digest,
                                       RUDP_WINDOW_CAPACITY,
                                       RUDP_INITIAL_RECEIVE_LIMIT) != RUDP_TRANSFER_OK) {
            return -1;
        }
    } else {
        const struct rudp_transfer_sink sink = {
            &cli->output,
            rudp_output_append,
            rudp_output_finish,
        };

        cli->receiver = calloc(1U, sizeof(*cli->receiver));
        if (cli->receiver == NULL ||
            rudp_windowed_receiver_start(cli->receiver, clock, io, &cli->session.peer,
                                         cli->session.client_nonce, cli->session.server_nonce,
                                         &cli->session.metadata, &sink) != RUDP_TRANSFER_OK) {
            return -1;
        }
    }
    cli->transfer_started = true;
    return 0;
}

static int deliver_packet(struct cli_context *cli, const struct rudp_peer *peer,
                          const struct rudp_packet *packet)
{
    if (!cli->transfer_started) {
        enum rudp_session_error error = rudp_session_receive(&cli->session, peer, packet);

        return error == RUDP_SESSION_OK || error == RUDP_SESSION_ERR_PEER ||
                       error == RUDP_SESSION_ERR_PACKET || error == RUDP_SESSION_ERR_STATE
                   ? 0
                   : -1;
    }
    if (!peer_equal(peer, &cli->session.peer)) {
        return 0;
    }
    if (packet->type == RUDP_PACKET_ABORT) {
        return -1;
    }
    if (cli->sending) {
        enum rudp_transfer_error error = rudp_windowed_sender_receive(cli->sender, packet);
        return error == RUDP_TRANSFER_OK || error == RUDP_TRANSFER_ERR_PACKET ? 0 : -1;
    }
    {
        enum rudp_transfer_error error = rudp_windowed_receiver_receive(cli->receiver, packet);
        return error == RUDP_TRANSFER_OK || error == RUDP_TRANSFER_ERR_PACKET ? 0 : -1;
    }
}

static int run(struct cli_context *cli)
{
    const struct rudp_clock clock = {NULL, monotonic_ms};
    const struct rudp_session_io io = {cli, counted_send};
    const struct rudp_random random = {NULL, secure_random};

    if (cli->sending) {
        struct rudp_transfer_metadata metadata = {.length = cli->source.length};

        memcpy(metadata.digest, cli->source.digest, sizeof(metadata.digest));
        if (rudp_sender_start(&cli->session, &clock, &random, &io, &cli->peer, &metadata) !=
            RUDP_SESSION_OK) {
            return -1;
        }
    } else if (rudp_receiver_listen(&cli->session, &clock, &random, &io) != RUDP_SESSION_OK) {
        return -1;
    }
    for (;;) {
        struct pollfd descriptor = {.fd = cli->socket.fd, .events = POLLIN};
        int ready;

        do {
            ready = poll(&descriptor, 1U, 10);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0 || (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            return -1;
        }
        for (;;) {
            struct rudp_peer peer;
            struct rudp_packet packet;
            enum rudp_socket_error error = rudp_socket_receive_packet(&cli->socket, &peer, &packet);

            if (error == RUDP_SOCKET_WOULD_BLOCK) {
                break;
            }
            if (error == RUDP_SOCKET_ERR_PACKET || error == RUDP_SOCKET_ERR_TRUNCATED) {
                cli->malformed_packets += 1U;
                continue;
            }
            if (error != RUDP_SOCKET_OK) {
                return -1;
            }
            cli->packets_received += 1U;
            if (deliver_packet(cli, &peer, &packet) != 0) {
                return -1;
            }
        }
        if (!cli->transfer_started) {
            if (rudp_session_tick(&cli->session) != RUDP_SESSION_OK ||
                start_transfer(cli, &clock, &io) != 0) {
                return -1;
            }
            continue;
        }
        if (cli->sending) {
            if (rudp_windowed_sender_tick(cli->sender) != RUDP_TRANSFER_OK) {
                return -1;
            }
            if (cli->sender->state == RUDP_TRANSFER_COMPLETE) {
                return rudp_source_verify(&cli->source);
            }
        } else {
            size_t consumed;

            if (rudp_windowed_receiver_consume(cli->receiver, RUDP_WINDOW_CAPACITY, &consumed) !=
                    RUDP_TRANSFER_OK ||
                rudp_windowed_receiver_tick(cli->receiver) != RUDP_TRANSFER_OK) {
                return -1;
            }
            if (cli->receiver->state == RUDP_TRANSFER_COMPLETE) {
                return 0;
            }
        }
    }
}

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s send HOST PORT INPUT\n       %s receive PORT OUTPUT\n", program,
            program);
}

int main(int argc, char **argv)
{
    struct cli_context cli;
    uint16_t port;
    int result;

    memset(&cli, 0, sizeof(cli));
    cli.socket.fd = -1;
    cli.output.fd = -1;
    if (argc == 5 && strcmp(argv[1], "send") == 0) {
        cli.sending = true;
        if (parse_port(argv[3], &port) != 0 || resolve_peer(argv[2], port, &cli.peer) != 0 ||
            rudp_source_load(&cli.source, argv[4]) != 0 ||
            rudp_socket_open(&cli.socket, 0U) != RUDP_SOCKET_OK) {
            print_status(&cli, false, "initialization", strerror(errno));
            rudp_source_release(&cli.source);
            return 1;
        }
    } else if (argc == 4 && strcmp(argv[1], "receive") == 0) {
        if (parse_port(argv[2], &port) != 0 || rudp_output_open(&cli.output, argv[3]) != 0 ||
            rudp_socket_open(&cli.socket, port) != RUDP_SOCKET_OK) {
            print_status(&cli, false, "initialization", strerror(errno));
            rudp_output_abort(&cli.output);
            return 1;
        }
    } else {
        usage(argv[0]);
        return 2;
    }
    result = run(&cli);
    if (result == 0) {
        print_status(&cli, true, "complete", "none");
    } else if (cli.transfer_started && cli.sending && cli.sender != NULL) {
        print_status(&cli, false, "transfer", transfer_error_string(cli.sender->error));
    } else if (cli.transfer_started && !cli.sending && cli.receiver != NULL) {
        print_status(&cli, false, "transfer", transfer_error_string(cli.receiver->error));
    } else {
        print_status(&cli, false, "session", rudp_session_error_string(cli.session.error));
    }
    rudp_socket_close(&cli.socket);
    rudp_source_release(&cli.source);
    rudp_output_abort(&cli.output);
    free(cli.sender);
    free(cli.receiver);
    return result == 0 ? 0 : 1;
}
