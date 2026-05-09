#include "rudp/window.h"

#include <string.h>

bool rudp_seq_after(uint32_t left, uint32_t right)
{
    const uint32_t distance = left - right;
    return distance != 0U && distance < UINT32_C(0x80000000);
}

bool rudp_seq_before(uint32_t left, uint32_t right)
{
    return rudp_seq_after(right, left);
}

bool rudp_seq_in_window(uint32_t sequence, uint32_t left, uint32_t right)
{
    const uint32_t width = right - left;
    const uint32_t distance = sequence - left;

    return width < UINT32_C(0x80000000) && distance < width;
}

void rudp_receive_window_init(struct rudp_receive_window *window, uint32_t initial_sequence)
{
    memset(window, 0, sizeof(*window));
    window->expected = initial_sequence;
    window->consumed = initial_sequence;
}

bool rudp_receive_window_insert(struct rudp_receive_window *window, uint32_t sequence,
                                uint16_t length)
{
    struct rudp_receive_slot *slot;

    if (!rudp_seq_in_window(sequence, window->consumed, window->consumed + RUDP_WINDOW_CAPACITY)) {
        return false;
    }
    slot = &window->slots[sequence % RUDP_WINDOW_CAPACITY];
    if (slot->present) {
        return slot->sequence == sequence;
    }
    slot->sequence = sequence;
    slot->length = length;
    slot->present = true;
    while (window->slots[window->expected % RUDP_WINDOW_CAPACITY].present &&
           window->slots[window->expected % RUDP_WINDOW_CAPACITY].sequence == window->expected) {
        window->expected += 1U;
    }
    return true;
}

uint32_t rudp_receive_window_limit(const struct rudp_receive_window *window)
{
    return window->consumed + RUDP_WINDOW_CAPACITY;
}

uint8_t rudp_receive_window_sacks(const struct rudp_receive_window *window,
                                  struct rudp_sack_block blocks[RUDP_MAX_SACK_BLOCKS])
{
    uint32_t sequence = window->expected + 1U;
    uint8_t count = 0U;

    while (
        rudp_seq_in_window(sequence, window->expected, window->consumed + RUDP_WINDOW_CAPACITY) &&
        count < RUDP_MAX_SACK_BLOCKS) {
        const struct rudp_receive_slot *slot = &window->slots[sequence % RUDP_WINDOW_CAPACITY];
        if (slot->present && slot->sequence == sequence) {
            blocks[count].start = sequence;
            do {
                sequence += 1U;
                slot = &window->slots[sequence % RUDP_WINDOW_CAPACITY];
            } while (rudp_seq_in_window(sequence, window->expected,
                                        window->consumed + RUDP_WINDOW_CAPACITY) &&
                     slot->present && slot->sequence == sequence);
            blocks[count].end = sequence;
            count += 1U;
        } else {
            sequence += 1U;
        }
    }
    return count;
}
