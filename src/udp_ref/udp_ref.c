#include "rudp/benchmark.h"
#include "rudp/io.h"
#include "rudp/record.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define UDP_REF_RECORD_SIZE 1024U
#define UDP_REF_BODY_OFFSET 20U
#define UDP_REF_WIRE_BYTES 1052U
#define UDP_REF_DEFAULT_RATE UINT64_C(20000000)
#define UDP_REF_CONTROL_SIZE 12U
#define UDP_REF_SUMMARY_SIZE 44U
#define UDP_REF_DRAIN_MS 1000U
#define UDP_REF_IDLE_TIMEOUT_MS 30000

struct udp_metrics {
    bool sending;
    uint64_t offered;
    uint64_t received;
    uint64_t invalid;
    uint64_t duplicates;
    uint64_t unique_bytes;
    uint64_t missing;
    int send_buffer;
    int receive_buffer;
    struct rudp_benchmark_clock benchmark_start;
};

static uint64_t monotonic_ms(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0U;
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

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

static int parse_u64(const char *text, uint64_t minimum, uint64_t maximum, uint64_t *output)
{
    char *end = NULL;
    unsigned long long value;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < minimum || value > maximum) {
        errno = EINVAL;
        return -1;
    }
    *output = (uint64_t)value;
    return 0;
}

static int resolve(const char *host, uint16_t port, int socket_type, struct sockaddr_in *result)
{
    struct addrinfo hints = {0};
    struct addrinfo *addresses = NULL;
    char service[6];
    hints.ai_family = AF_INET;
    hints.ai_socktype = socket_type;
    (void)snprintf(service, sizeof(service), "%u", (unsigned int)port);
    if (getaddrinfo(host, service, &hints, &addresses) != 0 || addresses == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (addresses->ai_addrlen != sizeof(*result)) {
        freeaddrinfo(addresses);
        errno = EINVAL;
        return -1;
    }
    memcpy(result, addresses->ai_addr, sizeof(*result));
    freeaddrinfo(addresses);
    return 0;
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

static bool valid_record(const uint8_t record[UDP_REF_RECORD_SIZE], uint64_t count, uint64_t *id)
{
    *id = rudp_record_id(record);
    if (*id >= count)
        return false;
    return rudp_record_validate(record, *id, NULL);
}

static int open_control_listener(uint16_t port)
{
    struct sockaddr_in address = {0};
    int enabled = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0 ||
        bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 1) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

static int open_udp_receiver(uint16_t port, struct udp_metrics *metrics)
{
    struct sockaddr_in address = {0};
    socklen_t length = sizeof(int);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        (void)close(fd);
        return -1;
    }
    (void)getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &metrics->receive_buffer, &length);
    length = sizeof(int);
    (void)getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &metrics->send_buffer, &length);
    return fd;
}

static int connect_control(const char *host, uint16_t port)
{
    struct sockaddr_in address;
    int fd;
    if (resolve(host, port, SOCK_STREAM, &address) != 0)
        return -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        if (fd >= 0)
            (void)close(fd);
        return -1;
    }
    return fd;
}

static int run_sender(const char *host, uint16_t data_port, uint16_t control_port, uint64_t count,
                      uint64_t rate_bps, struct udp_metrics *metrics)
{
    struct sockaddr_in address;
    uint8_t control[UDP_REF_CONTROL_SIZE] = {'R', 'D', 'C', '1'};
    uint8_t summary[UDP_REF_SUMMARY_SIZE];
    uint8_t record[UDP_REF_RECORD_SIZE];
    uint64_t interval_ns = (UINT64_C(1000000000) * 8U * UDP_REF_WIRE_BYTES) / rate_bps;
    uint64_t next_send_ns;
    uint64_t id;
    int udp_fd = -1;
    int control_fd = -1;
    socklen_t length = sizeof(int);
    int result = -1;

    if (resolve(host, data_port, SOCK_DGRAM, &address) != 0 ||
        (control_fd = connect_control(host, control_port)) < 0)
        goto done;
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0 || connect(udp_fd, (const struct sockaddr *)&address, sizeof(address)) != 0)
        goto done;
    (void)getsockopt(udp_fd, SOL_SOCKET, SO_SNDBUF, &metrics->send_buffer, &length);
    length = sizeof(int);
    (void)getsockopt(udp_fd, SOL_SOCKET, SO_RCVBUF, &metrics->receive_buffer, &length);
    put_u64(control + 4U, count);
    if (rudp_write_all(socket_write, &control_fd, control, sizeof(control)) != 0)
        goto done;
    if (interval_ns == 0U) {
        errno = ERANGE;
        goto done;
    }
    next_send_ns = monotonic_ns();
    for (id = 0U; id < count; ++id) {
        struct timespec deadline;
        next_send_ns += interval_ns;
        deadline.tv_sec = (time_t)(next_send_ns / UINT64_C(1000000000));
        deadline.tv_nsec = (long)(next_send_ns % UINT64_C(1000000000));
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) == EINTR)
            ;
        if (monotonic_ns() > next_send_ns + interval_ns * 2U) {
            /* Never compensate a scheduler stall with an unbounded burst. */
            next_send_ns = monotonic_ns();
        }
        rudp_record_make(record, id, monotonic_ns());
        if (send(udp_fd, record, sizeof(record), 0) != (ssize_t)sizeof(record))
            goto done;
        metrics->offered += 1U;
    }
    if (shutdown(control_fd, SHUT_WR) != 0 || read_all(control_fd, summary, sizeof(summary)) != 0 ||
        memcmp(summary, "RDS1", 4U) != 0)
        goto done;
    metrics->received = get_u64(summary + 12U);
    metrics->invalid = get_u64(summary + 20U);
    metrics->duplicates = get_u64(summary + 28U);
    metrics->missing = get_u64(summary + 36U);
    metrics->unique_bytes = metrics->received * UDP_REF_RECORD_SIZE;
    result = 0;
done:
    if (udp_fd >= 0)
        (void)close(udp_fd);
    if (control_fd >= 0)
        (void)close(control_fd);
    return result;
}

static bool bit_is_set(const uint8_t *bits, uint64_t id)
{
    return (bits[id / 8U] & (uint8_t)(1U << (id % 8U))) != 0U;
}

static void set_bit(uint8_t *bits, uint64_t id)
{
    bits[id / 8U] |= (uint8_t)(1U << (id % 8U));
}

static int run_receiver(uint16_t data_port, uint16_t control_port, struct udp_metrics *metrics)
{
    uint8_t control[UDP_REF_CONTROL_SIZE];
    uint8_t summary[UDP_REF_SUMMARY_SIZE] = {'R', 'D', 'S', '1'};
    uint8_t record[UDP_REF_RECORD_SIZE];
    uint8_t *seen = NULL;
    uint64_t count;
    uint64_t drain_deadline = 0U;
    int listener = -1;
    int control_fd = -1;
    int udp_fd = -1;
    int result = -1;

    listener = open_control_listener(control_port);
    udp_fd = open_udp_receiver(data_port, metrics);
    if (listener < 0 || udp_fd < 0)
        goto done;
    control_fd = accept(listener, NULL, NULL);
    (void)close(listener);
    listener = -1;
    if (control_fd < 0 || read_all(control_fd, control, sizeof(control)) != 0 ||
        memcmp(control, "RDC1", 4U) != 0)
        goto done;
    count = get_u64(control + 4U);
    if (count == 0U || count > UINT64_C(16777216)) {
        errno = EFBIG;
        goto done;
    }
    seen = calloc((size_t)((count + 7U) / 8U), 1U);
    if (seen == NULL)
        goto done;
    for (;;) {
        struct pollfd descriptors[2] = {{udp_fd, POLLIN, 0}, {control_fd, POLLIN | POLLHUP, 0}};

        if (drain_deadline != 0U && monotonic_ms() >= drain_deadline)
            break;
        int timeout =
            drain_deadline == 0U
                ? UDP_REF_IDLE_TIMEOUT_MS
                : (monotonic_ms() >= drain_deadline ? 0 : (int)(drain_deadline - monotonic_ms()));
        int ready = poll(descriptors, 2U, timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            goto done;
        if (ready == 0) {
            if (drain_deadline != 0U)
                break;
            errno = ETIMEDOUT;
            goto done;
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            ssize_t amount = recv(udp_fd, record, sizeof(record), MSG_TRUNC);
            uint64_t id;
            if (amount != (ssize_t)sizeof(record) || !valid_record(record, count, &id)) {
                metrics->invalid += 1U;
            } else if (bit_is_set(seen, id)) {
                metrics->duplicates += 1U;
            } else {
                set_bit(seen, id);
                metrics->received += 1U;
                metrics->unique_bytes += UDP_REF_RECORD_SIZE;
            }
        }
        if (drain_deadline == 0U && (descriptors[1].revents & (POLLIN | POLLHUP)) != 0) {
            uint8_t byte;
            ssize_t amount = recv(control_fd, &byte, 1U, 0);
            if (amount < 0 && errno != EINTR)
                goto done;
            if (amount == 0)
                drain_deadline = monotonic_ms() + UDP_REF_DRAIN_MS;
        }
    }
    metrics->missing = count - metrics->received;
    put_u64(summary + 4U, count);
    put_u64(summary + 12U, metrics->received);
    put_u64(summary + 20U, metrics->invalid);
    put_u64(summary + 28U, metrics->duplicates);
    put_u64(summary + 36U, metrics->missing);
    if (rudp_write_all(socket_write, &control_fd, summary, sizeof(summary)) != 0)
        goto done;
    result = 0;
done:
    free(seen);
    if (listener >= 0)
        (void)close(listener);
    if (control_fd >= 0)
        (void)close(control_fd);
    if (udp_fd >= 0)
        (void)close(udp_fd);
    return result;
}

static void print_status(const struct udp_metrics *metrics, bool success, const char *error)
{
    struct rudp_benchmark_clock end = {0};
    (void)rudp_benchmark_clock_read(&end);
    const struct rudp_benchmark_record record = {
        .tool = "udp_ref",
        .role = metrics->sending ? "sender" : "receiver",
        .status = success ? "success" : "failure",
        .stage = success ? "complete" : "transfer",
        .error = error,
        .bytes = (metrics->sending ? metrics->offered : metrics->received) * UDP_REF_RECORD_SIZE,
        .packets_sent = metrics->sending ? metrics->offered : 0U,
        .packets_received = metrics->received + metrics->duplicates + metrics->invalid,
        .malformed_packets = metrics->invalid,
        .unique_bytes = metrics->unique_bytes,
        .duplicate_records = metrics->duplicates,
        .missing_records = metrics->missing,
        .socket_send_buffer = metrics->send_buffer,
        .socket_receive_buffer = metrics->receive_buffer,
        .started_ns = metrics->benchmark_start.monotonic_ns,
        .ended_ns = end.monotonic_ns,
        .user_cpu_ns = end.user_cpu_ns - metrics->benchmark_start.user_cpu_ns,
        .system_cpu_ns = end.system_cpu_ns - metrics->benchmark_start.system_cpu_ns,
    };
    (void)rudp_benchmark_record_write(stdout, &record);
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s send HOST DATA_PORT CONTROL_PORT RECORD_COUNT [RATE_BPS]\n"
            "       %s receive DATA_PORT CONTROL_PORT\n",
            program, program);
}

int main(int argc, char **argv)
{
    struct udp_metrics metrics = {0};
    uint64_t data_port_value;
    uint64_t control_port_value;
    uint64_t count;
    uint64_t rate = UDP_REF_DEFAULT_RATE;
    int result;
    (void)rudp_benchmark_clock_read(&metrics.benchmark_start);
    if ((argc == 6 || argc == 7) && strcmp(argv[1], "send") == 0) {
        metrics.sending = true;
        if (parse_u64(argv[3], 1U, UINT16_MAX, &data_port_value) != 0 ||
            parse_u64(argv[4], 1U, UINT16_MAX, &control_port_value) != 0 ||
            parse_u64(argv[5], 1U, UINT64_C(16777216), &count) != 0 ||
            (argc == 7 && parse_u64(argv[6], 1U, UINT64_MAX, &rate) != 0)) {
            usage(argv[0]);
            return 2;
        }
        result = run_sender(argv[2], (uint16_t)data_port_value, (uint16_t)control_port_value, count,
                            rate, &metrics);
    } else if (argc == 4 && strcmp(argv[1], "receive") == 0) {
        if (parse_u64(argv[2], 1U, UINT16_MAX, &data_port_value) != 0 ||
            parse_u64(argv[3], 1U, UINT16_MAX, &control_port_value) != 0) {
            usage(argv[0]);
            return 2;
        }
        result = run_receiver((uint16_t)data_port_value, (uint16_t)control_port_value, &metrics);
    } else {
        usage(argv[0]);
        return 2;
    }
    print_status(&metrics, result == 0, result == 0 ? "none" : strerror(errno));
    return result == 0 ? 0 : 1;
}
