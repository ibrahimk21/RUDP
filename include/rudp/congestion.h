#ifndef RUDP_CONGESTION_H
#define RUDP_CONGESTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RUDP_AIMD_INITIAL_WINDOW 10U
#define RUDP_SAT_RING_CAPACITY 50U

enum rudp_cc_algorithm {
    RUDP_CC_AIMD,
    RUDP_CC_SAT,
};

struct rudp_sat_entry {
    uint32_t sequence;
    bool occupied;
    bool marked_lost;
};

struct rudp_fixed_cc {
    uint32_t window;
};

struct rudp_aimd_cc {
    double cwnd;
    double ssthresh;
    uint32_t maximum_window;
    uint32_t recovery_boundary;
    uint64_t last_data_send_ms;
    bool in_recovery;
    bool has_sent;
    bool sat_enabled;
    uint32_t sat_next;
    uint32_t sat_occupied;
    uint32_t sat_marked;
    struct rudp_sat_entry sat_ring[RUDP_SAT_RING_CAPACITY];
};

struct rudp_pacer {
    double credit_packets;
    uint64_t updated_ms;
    bool initialized;
};

void rudp_fixed_cc_init(struct rudp_fixed_cc *cc, uint32_t window);
uint32_t rudp_fixed_cc_window(const struct rudp_fixed_cc *cc);
void rudp_aimd_init(struct rudp_aimd_cc *cc, uint32_t maximum_window);
uint32_t rudp_aimd_window(const struct rudp_aimd_cc *cc);
void rudp_aimd_on_ack(struct rudp_aimd_cc *cc, uint32_t newly_acked, uint32_t cumulative_ack);
void rudp_aimd_on_fast_loss(struct rudp_aimd_cc *cc, size_t flight, uint32_t recovery_boundary);
void rudp_aimd_on_timeout(struct rudp_aimd_cc *cc, size_t flight, uint32_t recovery_boundary);
void rudp_aimd_on_data_send(struct rudp_aimd_cc *cc, uint64_t now_ms);
void rudp_aimd_resume_after_idle(struct rudp_aimd_cc *cc, uint64_t now_ms, double rto_ms,
                                 bool credit_limited);
void rudp_sat_init(struct rudp_aimd_cc *cc, uint32_t maximum_window);
void rudp_sat_on_original_send(struct rudp_aimd_cc *cc, uint32_t sequence, uint64_t now_ms,
                               size_t flight, uint32_t recovery_boundary);
void rudp_sat_on_fast_loss(struct rudp_aimd_cc *cc, uint32_t sequence, size_t flight,
                           uint32_t recovery_boundary);
void rudp_pacer_init(struct rudp_pacer *pacer, uint64_t now_ms);
bool rudp_pacer_take(struct rudp_pacer *pacer, uint64_t now_ms, double cwnd, double srtt_ms,
                     size_t payload_bytes);
uint64_t rudp_pacer_next_ms(const struct rudp_pacer *pacer, uint64_t now_ms, double cwnd,
                            double srtt_ms, size_t payload_bytes);

#endif
