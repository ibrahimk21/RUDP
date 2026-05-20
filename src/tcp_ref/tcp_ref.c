#include "rudp/benchmark.h"
#include "rudp/file.h"
#include "rudp/io.h"
#include "rudp/md5.h"
#include "rudp/record.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/tcp.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define TCP_REF_HEADER_SIZE 28U
#define TCP_REF_TIMEOUT_SECONDS 30

struct tcp_context {
    bool sending;
    bool streaming;
    const char *requested_cc;
    char actual_cc[64];
    uint64_t bytes;
    int send_buffer;
    int receive_buffer;
    struct tcp_info info;
    struct rudp_benchmark_clock benchmark_start;
};

static uint64_t monotonic_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0U;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static void put_u64(uint8_t *output, uint64_t value)
{
    unsigned int index;
    for (index = 0U; index < 8U; ++index)
        output[index] = (uint8_t)(value >> (56U - index * 8U));
}

static uint64_t get_u64(const uint8_t *input)
{
    uint64_t value = 0U;
    unsigned int index;
    for (index = 0U; index < 8U; ++index)
        value = (value << 8U) | input[index];
    return value;
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

static int set_timeout(int fd)
{
    const struct timeval timeout = {.tv_sec = TCP_REF_TIMEOUT_SECONDS, .tv_usec = 0};
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0 &&
                   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0
               ? 0
               : -1;
}

static int select_cc(int fd, const char *requested, char actual[64])
{
    socklen_t length = 64U;
    if (strcmp(requested, "cubic") != 0 && strcmp(requested, "bbr") != 0) {
        errno = EINVAL;
        return -1;
    }
    if (setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, requested, (socklen_t)strlen(requested)) != 0 ||
        getsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, actual, &length) != 0)
        return -1;
    if (length >= 64U)
        length = 63U;
    actual[length] = '\0';
    if (strcmp(actual, requested) != 0) {
        errno = ENOPROTOOPT;
        return -1;
    }
    return 0;
}

static int tcp_socket(const char *cc, char actual[64])
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || set_timeout(fd) != 0 || select_cc(fd, cc, actual) != 0) {
        if (fd >= 0)
            (void)close(fd);
        return -1;
    }
    return fd;
}

static int connect_to(const char *host, uint16_t port, const char *cc, char actual[64])
{
    struct addrinfo hints = {0};
    struct addrinfo *addresses = NULL;
    struct addrinfo *current;
    char service[6];
    int fd = -1;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    (void)snprintf(service, sizeof(service), "%u", (unsigned int)port);
    if (getaddrinfo(host, service, &hints, &addresses) != 0) {
        errno = EINVAL;
        return -1;
    }
    for (current = addresses; current != NULL; current = current->ai_next) {
        fd = tcp_socket(cc, actual);
        if (fd >= 0 && connect(fd, current->ai_addr, current->ai_addrlen) == 0)
            break;
        if (fd >= 0)
            (void)close(fd);
        fd = -1;
        if (errno == ENOENT || errno == ENOPROTOOPT)
            break;
    }
    freeaddrinfo(addresses);
    return fd;
}

static int accept_one(uint16_t port, const char *cc, char actual[64])
{
    struct sockaddr_in address = {0};
    int listener = tcp_socket(cc, actual);
    int enabled = 1;
    int fd;
    if (listener < 0)
        return -1;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0 ||
        bind(listener, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        (void)close(listener);
        return -1;
    }
    {
        struct pollfd wait_for_peer = {listener, POLLIN, 0};
        int poll_result;

        do {
            poll_result = poll(&wait_for_peer, 1U, TCP_REF_TIMEOUT_SECONDS * 1000);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result <= 0) {
            if (poll_result == 0)
                errno = ETIMEDOUT;
            (void)close(listener);
            return -1;
        }
    }
    fd = accept(listener, NULL, NULL);
    (void)close(listener);
    if (fd < 0 || set_timeout(fd) != 0 || select_cc(fd, cc, actual) != 0) {
        if (fd >= 0)
            (void)close(fd);
        return -1;
    }
    return fd;
}

static ssize_t socket_write(void *context, const uint8_t *bytes, size_t length)
{
    int fd = *(int *)context;
    return send(fd, bytes, length, MSG_NOSIGNAL);
}

static int read_all(int fd, uint8_t *bytes, size_t length)
{
    size_t offset = 0U;
    while (offset != length) {
        ssize_t count = recv(fd, bytes + offset, length - offset, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            if (count == 0)
                errno = ECONNRESET;
            return -1;
        }
        offset += (size_t)count;
    }
    return 0;
}

static int send_file(int fd, const char *path, struct tcp_context *context)
{
    struct rudp_source_file source;
    uint8_t header[TCP_REF_HEADER_SIZE] = {'R', 'D', 'T', '1'};
    int result = -1;
    if (rudp_source_load(&source, path) != 0)
        return -1;
    put_u64(header + 4U, source.length);
    memcpy(header + 12U, source.digest, 16U);
    if (rudp_write_all(socket_write, &fd, header, sizeof(header)) == 0 &&
        rudp_write_all(socket_write, &fd, source.bytes, source.length) == 0 &&
        shutdown(fd, SHUT_WR) == 0 && rudp_source_verify(&source) == 0) {
        context->bytes = source.length;
        result = 0;
    }
    rudp_source_release(&source);
    return result;
}

static int send_stream(int fd, uint64_t duration_ms, struct tcp_context *context)
{
    uint8_t header[TCP_REF_HEADER_SIZE] = {'R', 'D', 'T', '2'};
    uint8_t record[RUDP_BENCHMARK_RECORD_SIZE];
    uint8_t final_record[RUDP_BENCHMARK_RECORD_SIZE] = {0};
    struct rudp_md5 md5;
    uint8_t digest[16];
    uint64_t id = 0U;
    uint64_t end_ns;

    put_u64(header + 4U, UINT64_MAX);
    if (rudp_write_all(socket_write, &fd, header, sizeof(header)) != 0)
        return -1;
    end_ns = monotonic_ns() + duration_ms * 1000000U;
    rudp_md5_init(&md5);
    while (monotonic_ns() < end_ns && id < RUDP_MAX_TRANSFER_LENGTH / RUDP_BENCHMARK_RECORD_SIZE) {
        rudp_record_make(record, id, monotonic_ns());
        if (rudp_write_all(socket_write, &fd, record, sizeof(record)) != 0)
            return -1;
        rudp_md5_update(&md5, record, sizeof(record));
        id += 1U;
    }
    rudp_md5_final(&md5, digest);
    put_u64(final_record, UINT64_MAX);
    put_u64(final_record + 8U, id * RUDP_BENCHMARK_RECORD_SIZE);
    memcpy(final_record + 16U, digest, sizeof(digest));
    if (rudp_write_all(socket_write, &fd, final_record, sizeof(final_record)) != 0 ||
        shutdown(fd, SHUT_WR) != 0)
        return -1;
    context->bytes = id * RUDP_BENCHMARK_RECORD_SIZE;
    return 0;
}

static int receive_stream(int fd, struct tcp_context *context)
{
    uint8_t record[RUDP_BENCHMARK_RECORD_SIZE];
    struct rudp_md5 md5;
    uint64_t expected_id = 0U;

    rudp_md5_init(&md5);
    for (;;) {
        if (read_all(fd, record, sizeof(record)) != 0)
            return -1;
        if (rudp_record_id(record) == UINT64_MAX) {
            struct rudp_md5 copy = md5;
            uint8_t digest[16];
            rudp_md5_final(&copy, digest);
            if (get_u64(record + 8U) != expected_id * RUDP_BENCHMARK_RECORD_SIZE ||
                memcmp(record + 16U, digest, sizeof(digest)) != 0) {
                errno = EBADMSG;
                return -1;
            }
            context->bytes = expected_id * RUDP_BENCHMARK_RECORD_SIZE;
            return 0;
        }
        if (!rudp_record_validate(record, expected_id, NULL)) {
            errno = EBADMSG;
            return -1;
        }
        rudp_md5_update(&md5, record, sizeof(record));
        expected_id += 1U;
    }
}

static int receive_file(int fd, const char *path, struct tcp_context *context)
{
    struct rudp_output_file output;
    uint8_t header[TCP_REF_HEADER_SIZE];
    uint8_t buffer[65536];
    uint64_t remaining;
    int result = -1;
    memset(&output, 0, sizeof(output));
    output.fd = -1;
    if (read_all(fd, header, sizeof(header)) != 0) {
        return -1;
    }
    if (memcmp(header, "RDT2", 4U) == 0 && get_u64(header + 4U) == UINT64_MAX)
        return receive_stream(fd, context);
    if (memcmp(header, "RDT1", 4U) != 0) {
        errno = EBADMSG;
        return -1;
    }
    remaining = get_u64(header + 4U);
    if (remaining > RUDP_MAX_TRANSFER_LENGTH) {
        errno = EFBIG;
        return -1;
    }
    if (rudp_output_open(&output, path) != 0)
        return -1;
    while (remaining != 0U) {
        size_t amount = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        if (read_all(fd, buffer, amount) != 0 || rudp_output_append(&output, buffer, amount) != 0)
            goto done;
        remaining -= amount;
    }
    if (rudp_output_finish(&output, output.length, header + 12U) == 0) {
        context->bytes = output.length;
        result = 0;
    }
done:
    rudp_output_abort(&output);
    return result;
}

static void collect_socket_info(int fd, struct tcp_context *context)
{
    socklen_t length = sizeof(context->info);
    socklen_t integer_length = sizeof(int);
    (void)getsockopt(fd, IPPROTO_TCP, TCP_INFO, &context->info, &length);
    (void)getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &context->send_buffer, &integer_length);
    integer_length = sizeof(int);
    (void)getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &context->receive_buffer, &integer_length);
}

static void status(const struct tcp_context *context, bool success, const char *stage,
                   const char *error)
{
    struct rudp_benchmark_clock end = {0};
    (void)rudp_benchmark_clock_read(&end);
    const struct rudp_benchmark_record record = {
        .tool = "tcp_ref",
        .role = context->sending ? "sender" : "receiver",
        .status = success ? "success" : "failure",
        .stage = stage,
        .error = error,
        .cc_requested = context->requested_cc,
        .cc_actual = context->actual_cc,
        .bytes = context->bytes,
        .tcp_snd_cwnd = context->info.tcpi_snd_cwnd,
        .tcp_rtt_us = context->info.tcpi_rtt,
        .tcp_retransmits = context->info.tcpi_retransmits,
        .socket_send_buffer = context->send_buffer,
        .socket_receive_buffer = context->receive_buffer,
        .started_ns = context->benchmark_start.monotonic_ns,
        .ended_ns = end.monotonic_ns,
        .user_cpu_ns = end.user_cpu_ns - context->benchmark_start.user_cpu_ns,
        .system_cpu_ns = end.system_cpu_ns - context->benchmark_start.system_cpu_ns,
    };
    (void)rudp_benchmark_record_write(stdout, &record);
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s send HOST PORT INPUT {cubic|bbr}\n"
            "       %s stream HOST PORT DURATION_MS {cubic|bbr}\n"
            "       %s receive PORT OUTPUT {cubic|bbr}\n",
            program, program, program);
}

static int parse_duration(const char *text, uint64_t *duration_ms)
{
    char *end = NULL;
    unsigned long long value;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0U || value >= UINT64_C(1800000)) {
        errno = EINVAL;
        return -1;
    }
    *duration_ms = (uint64_t)value;
    return 0;
}

int main(int argc, char **argv)
{
    struct tcp_context context = {0};
    uint16_t port;
    int fd = -1;
    int result;
    int saved_error;
    uint64_t duration_ms = 0U;
    (void)signal(SIGPIPE, SIG_IGN);
    (void)rudp_benchmark_clock_read(&context.benchmark_start);
    if (argc == 6 && strcmp(argv[1], "send") == 0) {
        context.sending = true;
        context.requested_cc = argv[5];
        if (parse_port(argv[3], &port) == 0)
            fd = connect_to(argv[2], port, argv[5], context.actual_cc);
    } else if (argc == 6 && strcmp(argv[1], "stream") == 0) {
        context.sending = true;
        context.streaming = true;
        context.requested_cc = argv[5];
        if (parse_port(argv[3], &port) == 0 && parse_duration(argv[4], &duration_ms) == 0)
            fd = connect_to(argv[2], port, argv[5], context.actual_cc);
    } else if (argc == 5 && strcmp(argv[1], "receive") == 0) {
        context.requested_cc = argv[4];
        if (parse_port(argv[2], &port) == 0)
            fd = accept_one(port, argv[4], context.actual_cc);
    } else {
        usage(argv[0]);
        return 2;
    }
    if (fd < 0) {
        status(&context, false, "socket", strerror(errno));
        return 1;
    }
    result = context.sending ? (context.streaming ? send_stream(fd, duration_ms, &context)
                                                  : send_file(fd, argv[4], &context))
                             : receive_file(fd, argv[3], &context);
    saved_error = errno;
    collect_socket_info(fd, &context);
    (void)close(fd);
    status(&context, result == 0, result == 0 ? "complete" : "transfer",
           result == 0 ? "none" : strerror(saved_error));
    return result == 0 ? 0 : 1;
}
