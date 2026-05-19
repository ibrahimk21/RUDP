#ifndef RUDP_BENCHMARK_H
#define RUDP_BENCHMARK_H

#include <stdint.h>
#include <stdio.h>

struct rudp_benchmark_record {
    const char *tool;
    const char *role;
    const char *status;
    const char *stage;
    const char *error;
    const char *cc_requested;
    const char *cc_actual;
    uint64_t bytes;
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t malformed_packets;
    uint32_t fast_retransmits;
    uint32_t timeout_retransmits;
    uint32_t clean_rtt_samples;
    uint32_t suppressed_rtt_samples;
    uint32_t tcp_snd_cwnd;
    uint32_t tcp_rtt_us;
    uint32_t tcp_retransmits;
    int socket_send_buffer;
    int socket_receive_buffer;
};

int rudp_benchmark_record_write(FILE *stream, const struct rudp_benchmark_record *record);

#endif
