#ifndef RUDP_WINDOW_H
#define RUDP_WINDOW_H

#include "rudp/packet.h"

#include <stdbool.h>
#include <stdint.h>

#define RUDP_WINDOW_CAPACITY 8192U

bool rudp_seq_after(uint32_t left, uint32_t right);
bool rudp_seq_before(uint32_t left, uint32_t right);
bool rudp_seq_in_window(uint32_t sequence, uint32_t left, uint32_t right);

struct rudp_receive_slot {
    uint32_t sequence;
    uint16_t length;
    bool present;
};

struct rudp_receive_window {
    uint32_t expected;
    uint32_t consumed;
    struct rudp_receive_slot slots[RUDP_WINDOW_CAPACITY];
};

void rudp_receive_window_init(struct rudp_receive_window *window, uint32_t initial_sequence);
bool rudp_receive_window_insert(struct rudp_receive_window *window, uint32_t sequence,
                                uint16_t length);
uint32_t rudp_receive_window_limit(const struct rudp_receive_window *window);
bool rudp_receive_window_consume(struct rudp_receive_window *window, uint32_t sequence);
void rudp_receive_window_make_ack(const struct rudp_receive_window *window, uint64_t client_nonce,
                                  uint64_t server_nonce, struct rudp_packet *packet);
uint8_t rudp_receive_window_sacks(const struct rudp_receive_window *window,
                                  struct rudp_sack_block blocks[RUDP_MAX_SACK_BLOCKS]);

struct rudp_send_slot {
    uint32_t sequence;
    uint8_t higher_sack_evidence;
    bool in_use;
    bool sacked;
    bool fast_retransmitted;
};

struct rudp_send_scoreboard {
    uint32_t cumulative_ack;
    uint32_t receive_limit;
    struct rudp_send_slot slots[RUDP_WINDOW_CAPACITY];
};

void rudp_send_scoreboard_init(struct rudp_send_scoreboard *scoreboard, uint32_t initial_ack,
                               uint32_t receive_limit);
bool rudp_send_scoreboard_track(struct rudp_send_scoreboard *scoreboard, uint32_t sequence);
bool rudp_send_scoreboard_apply_ack(struct rudp_send_scoreboard *scoreboard, uint32_t ack,
                                    const struct rudp_sack_block *sacks, uint8_t sack_count,
                                    uint32_t *fast_retransmit_sequence);

#endif
