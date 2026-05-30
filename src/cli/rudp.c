#include "rudp/benchmark.h"
#include "rudp/file.h"
#include "rudp/record.h"
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
    bool streaming;
    bool transfer_started;
    uint64_t stream_duration_ms;
    uint64_t stream_interval_ms;
    uint64_t stream_records;
    uint64_t stream_bytes;
    struct rudp_md5 stream_md5;
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t malformed_packets;
    struct rudp_benchmark_clock benchmark_start;
    uint64_t ready_ns;
    uint64_t first_byte_ns;
    FILE *records;
    int socket_send_buffer;
    int socket_receive_buffer;
    enum rudp_cc_algorithm congestion_algorithm;
};

static const char *congestion_name(enum rudp_cc_algorithm algorithm)
{
    return algorithm == RUDP_CC_SAT ? "sat" : "aimd";
}

static void collect_socket_buffers(struct cli_context *cli)
{
    socklen_t length = sizeof(int);
    (void)getsockopt(cli->socket.fd, SOL_SOCKET, SO_SNDBUF, &cli->socket_send_buffer, &length);
    length = sizeof(int);
    (void)getsockopt(cli->socket.fd, SOL_SOCKET, SO_RCVBUF, &cli->socket_receive_buffer, &length);
}

static void log_delivery(struct cli_context *cli, const uint8_t *bytes, uint64_t record_id)
{
    uint64_t offered;
    struct rudp_benchmark_clock delivered;

    if (cli->records == NULL || !rudp_record_validate(bytes, record_id, &offered) ||
        rudp_benchmark_clock_read(&delivered) != 0)
        return;
    (void)fprintf(cli->records, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", record_id, offered,
                  delivered.monotonic_ns);
}

static int stream_append(void *context, const uint8_t *bytes, size_t length)
{
    struct cli_context *cli = context;
    if (length != RUDP_BENCHMARK_RECORD_SIZE ||
        !rudp_record_validate(bytes, cli->stream_records, NULL)) {
        errno = EBADMSG;
        return -1;
    }
    if (cli->first_byte_ns == 0U) {
        struct rudp_benchmark_clock first;
        if (rudp_benchmark_clock_read(&first) == 0)
            cli->first_byte_ns = first.monotonic_ns;
    }
    log_delivery(cli, bytes, cli->stream_records);
    rudp_md5_update(&cli->stream_md5, bytes, length);
    cli->stream_records += 1U;
    cli->stream_bytes += length;
    return 0;
}

static int file_append(void *context, const uint8_t *bytes, size_t length)
{
    struct cli_context *cli = context;

    if (length != 0U && cli->first_byte_ns == 0U) {
        struct rudp_benchmark_clock first;
        if (rudp_benchmark_clock_read(&first) == 0)
            cli->first_byte_ns = first.monotonic_ns;
    }
    if (cli->records != NULL) {
        if (length != RUDP_BENCHMARK_RECORD_SIZE ||
            cli->output.length % RUDP_BENCHMARK_RECORD_SIZE != 0U) {
            errno = EBADMSG;
            return -1;
        }
        log_delivery(cli, bytes, cli->output.length / RUDP_BENCHMARK_RECORD_SIZE);
    }
    return rudp_output_append(&cli->output, bytes, length);
}

static int file_finish(void *context, uint64_t length, const uint8_t digest[16])
{
    struct cli_context *cli = context;
    return rudp_output_finish(&cli->output, length, digest);
}

static int stream_finish(void *context, uint64_t length, const uint8_t digest[16])
{
    const struct cli_context *cli = context;
    struct rudp_md5 copy = cli->stream_md5;
    uint8_t actual[16];
    rudp_md5_final(&copy, actual);
    if (length != cli->stream_bytes || memcmp(actual, digest, sizeof(actual)) != 0) {
        errno = EBADMSG;
        return -1;
    }
    return 0;
}

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
    struct rudp_benchmark_clock end = {0};
    (void)rudp_benchmark_clock_read(&end);
    const struct rudp_benchmark_record record = {
        .tool = "rudp",
        .role = cli->sending ? "sender" : "receiver",
        .status = success ? "success" : "failure",
        .stage = stage,
        .error = error,
        .cc_requested = congestion_name(cli->congestion_algorithm),
        .cc_actual = congestion_name(cli->congestion_algorithm),
        .bytes = cli->streaming ? (cli->sending && cli->sender != NULL ? cli->sender->source_length
                                                                       : cli->stream_bytes)
                                : (cli->sending ? cli->source.length : cli->output.length),
        .packets_sent = cli->packets_sent,
        .packets_received = cli->packets_received,
        .malformed_packets = cli->malformed_packets,
        .fast_retransmits = cli->sender == NULL ? 0U : cli->sender->fast_retransmits,
        .timeout_retransmits = cli->sender == NULL ? 0U : cli->sender->timeout_retransmits,
        .clean_rtt_samples = cli->sender == NULL ? 0U : cli->sender->clean_rtt_samples,
        .suppressed_rtt_samples = cli->sender == NULL ? 0U : cli->sender->suppressed_rtt_samples,
        .started_ns = cli->benchmark_start.monotonic_ns,
        .ready_ns = cli->ready_ns,
        .first_byte_ns = cli->first_byte_ns,
        .ended_ns = end.monotonic_ns,
        .user_cpu_ns = end.user_cpu_ns - cli->benchmark_start.user_cpu_ns,
        .system_cpu_ns = end.system_cpu_ns - cli->benchmark_start.system_cpu_ns,
        .socket_send_buffer = cli->socket_send_buffer,
        .socket_receive_buffer = cli->socket_receive_buffer,
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
        if (cli->sender == NULL)
            return -1;
        if (cli->streaming) {
            if (rudp_windowed_sender_start_stream_with_cc(
                    cli->sender, clock, io, &cli->session.peer, cli->session.client_nonce,
                    cli->session.server_nonce, cli->stream_duration_ms, cli->stream_interval_ms,
                    RUDP_WINDOW_CAPACITY, RUDP_INITIAL_RECEIVE_LIMIT,
                    cli->congestion_algorithm) != RUDP_TRANSFER_OK)
                return -1;
        } else if (rudp_windowed_sender_start_with_cc(
                       cli->sender, clock, io, &cli->session.peer, cli->session.client_nonce,
                       cli->session.server_nonce, cli->source.bytes, cli->source.length,
                       cli->source.digest, RUDP_WINDOW_CAPACITY, RUDP_INITIAL_RECEIVE_LIMIT,
                       cli->congestion_algorithm) != RUDP_TRANSFER_OK) {
            return -1;
        }
    } else {
        struct rudp_transfer_sink sink;

        if ((cli->session.metadata.length == UINT64_MAX) != cli->streaming) {
            errno = EPROTO;
            return -1;
        }
        if (cli->streaming) {
            sink = (struct rudp_transfer_sink){cli, stream_append, stream_finish};
            rudp_md5_init(&cli->stream_md5);
        } else {
            sink = (struct rudp_transfer_sink){cli, file_append, file_finish};
        }

        cli->receiver = calloc(1U, sizeof(*cli->receiver));
        if (cli->receiver == NULL ||
            rudp_windowed_receiver_start(cli->receiver, clock, io, &cli->session.peer,
                                         cli->session.client_nonce, cli->session.server_nonce,
                                         &cli->session.metadata, &sink) != RUDP_TRANSFER_OK) {
            return -1;
        }
    }
    cli->transfer_started = true;
    if (cli->ready_ns == 0U) {
        struct rudp_benchmark_clock ready;
        if (rudp_benchmark_clock_read(&ready) == 0)
            cli->ready_ns = ready.monotonic_ns;
    }
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
        struct rudp_transfer_metadata metadata = {.length = cli->streaming ? UINT64_MAX
                                                                           : cli->source.length};

        if (!cli->streaming)
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
                return cli->streaming ? 0 : rudp_source_verify(&cli->source);
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
    fprintf(stderr,
            "usage: %s send HOST PORT INPUT\n"
            "       %s stream HOST PORT DURATION_MS\n"
            "       %s latency HOST PORT DURATION_MS\n"
            "       %s receive PORT OUTPUT_OR_DASH\n",
            program, program, program, program);
}

static int parse_duration(const char *text, uint64_t *duration)
{
    char *end = NULL;
    unsigned long long value;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0U ||
        value >= RUDP_TRANSFER_TIMEOUT_MS) {
        errno = EINVAL;
        return -1;
    }
    *duration = (uint64_t)value;
    return 0;
}

int main(int argc, char **argv)
{
    struct cli_context cli;
    uint16_t port;
    int result;

    memset(&cli, 0, sizeof(cli));
    if (getenv("RUDP_CC") != NULL && strcmp(getenv("RUDP_CC"), "sat") == 0) {
        cli.congestion_algorithm = RUDP_CC_SAT;
    } else if (getenv("RUDP_CC") != NULL && strcmp(getenv("RUDP_CC"), "aimd") != 0) {
        errno = EINVAL;
        print_status(&cli, false, "initialization", "RUDP_CC must be aimd or sat");
        return 1;
    }
    (void)rudp_benchmark_clock_read(&cli.benchmark_start);
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
    } else if (argc == 5 && (strcmp(argv[1], "stream") == 0 || strcmp(argv[1], "latency") == 0)) {
        cli.sending = true;
        cli.streaming = true;
        cli.stream_interval_ms = strcmp(argv[1], "latency") == 0 ? 20U : 0U;
        if (parse_port(argv[3], &port) != 0 || resolve_peer(argv[2], port, &cli.peer) != 0 ||
            parse_duration(argv[4], &cli.stream_duration_ms) != 0 ||
            rudp_socket_open(&cli.socket, 0U) != RUDP_SOCKET_OK) {
            print_status(&cli, false, "initialization", strerror(errno));
            return 1;
        }
    } else if (argc == 4 && strcmp(argv[1], "receive") == 0) {
        cli.streaming = strcmp(argv[3], "-") == 0;
        if (getenv("RUDP_RECORDS_CSV") != NULL) {
            cli.records = fopen(getenv("RUDP_RECORDS_CSV"), "w");
            if (cli.records != NULL)
                (void)fputs("record_id,offer_ns,delivered_ns\n", cli.records);
        }
        if (parse_port(argv[2], &port) != 0 ||
            (getenv("RUDP_RECORDS_CSV") != NULL && cli.records == NULL) ||
            (!cli.streaming && rudp_output_open(&cli.output, argv[3]) != 0) ||
            rudp_socket_open(&cli.socket, port) != RUDP_SOCKET_OK) {
            print_status(&cli, false, "initialization", strerror(errno));
            rudp_output_abort(&cli.output);
            return 1;
        }
    } else {
        usage(argv[0]);
        return 2;
    }
    collect_socket_buffers(&cli);
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
    if (cli.records != NULL)
        (void)fclose(cli.records);
    return result == 0 ? 0 : 1;
}
