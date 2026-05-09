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
uint8_t rudp_receive_window_sacks(const struct rudp_receive_window *window,
                                  struct rudp_sack_block blocks[RUDP_MAX_SACK_BLOCKS]);

#endif
