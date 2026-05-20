#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "rudp/record.h"

#define RECORD_SIZE 1024U

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

static void make_record(uint8_t record[RECORD_SIZE], uint64_t id)
{
    rudp_record_make(record, id, 123U + id);
}

static void send_record(int fd, const struct sockaddr_in *peer, uint64_t id, bool corrupt)
{
    uint8_t record[RECORD_SIZE];
    make_record(record, id);
    if (corrupt)
        record[100U] ^= 1U;
    assert(sendto(fd, record, sizeof(record), 0, (const struct sockaddr *)peer, sizeof(*peer)) ==
           (ssize_t)sizeof(record));
}

static int connect_retry(const struct sockaddr_in *peer)
{
    struct timespec pause = {0, 20000000L};
    unsigned int attempt;
    for (attempt = 0U; attempt < 50U; ++attempt) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        if (connect(fd, (const struct sockaddr *)peer, sizeof(*peer)) == 0)
            return fd;
        assert(close(fd) == 0);
        (void)nanosleep(&pause, NULL);
    }
    return -1;
}

int main(void)
{
    const char *build_dir = getenv("BUILD_DIR");
    char binary[512];
    char data_port_text[16];
    char control_port_text[16];
    uint16_t data_port = (uint16_t)(42000U + (unsigned int)getpid() % 10000U);
    uint16_t control_port = (uint16_t)(data_port + 1U);
    struct sockaddr_in data_peer = {0};
    struct sockaddr_in control_peer = {0};
    uint8_t control[12] = {'R', 'D', 'C', '1'};
    uint8_t summary[44];
    pid_t receiver;
    int receiver_status;
    int control_fd;
    int udp_fd;

    assert(snprintf(binary, sizeof(binary), "%s/udp_ref", build_dir == NULL ? "build" : build_dir) >
           0);
    assert(snprintf(data_port_text, sizeof(data_port_text), "%u", data_port) > 0);
    assert(snprintf(control_port_text, sizeof(control_port_text), "%u", control_port) > 0);
    receiver = fork();
    assert(receiver >= 0);
    if (receiver == 0) {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0)
            (void)dup2(null_fd, STDOUT_FILENO);
        execl(binary, binary, "receive", data_port_text, control_port_text, (char *)NULL);
        _exit(127);
    }
    control_peer.sin_family = AF_INET;
    control_peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    control_peer.sin_port = htons(control_port);
    control_fd = connect_retry(&control_peer);
    assert(control_fd >= 0);
    put_u64(control + 4U, 4U);
    assert(send(control_fd, control, sizeof(control), 0) == (ssize_t)sizeof(control));

    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(udp_fd >= 0);
    data_peer.sin_family = AF_INET;
    data_peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    data_peer.sin_port = htons(data_port);
    send_record(udp_fd, &data_peer, 0U, false);
    send_record(udp_fd, &data_peer, 1U, false);
    send_record(udp_fd, &data_peer, 1U, false);
    send_record(udp_fd, &data_peer, 3U, false);
    send_record(udp_fd, &data_peer, 0U, true);
    assert(close(udp_fd) == 0);
    assert(shutdown(control_fd, SHUT_WR) == 0);
    assert(recv(control_fd, summary, sizeof(summary), MSG_WAITALL) == (ssize_t)sizeof(summary));
    assert(memcmp(summary, "RDS1", 4U) == 0);
    assert(get_u64(summary + 4U) == 4U);
    assert(get_u64(summary + 12U) == 3U);
    assert(get_u64(summary + 20U) == 1U);
    assert(get_u64(summary + 28U) == 1U);
    assert(get_u64(summary + 36U) == 1U);
    assert(close(control_fd) == 0);
    assert(waitpid(receiver, &receiver_status, 0) == receiver);
    assert(WIFEXITED(receiver_status) && WEXITSTATUS(receiver_status) == 0);
    puts("raw UDP loss, duplicate, and validation accounting test passed");
    return 0;
}
