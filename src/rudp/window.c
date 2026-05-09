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

bool rudp_receive_window_consume(struct rudp_receive_window *window, uint32_t sequence)
{
    struct rudp_receive_slot *slot;

    if (sequence != window->consumed) {
        return false;
    }
    slot = &window->slots[sequence % RUDP_WINDOW_CAPACITY];
    if (!slot->present || slot->sequence != sequence) {
        return false;
    }
    slot->present = false;
    window->consumed += 1U;
    return true;
}

void rudp_receive_window_make_ack(const struct rudp_receive_window *window, uint64_t client_nonce,
                                  uint64_t server_nonce, struct rudp_packet *packet)
{
    memset(packet, 0, sizeof(*packet));
    packet->type = RUDP_PACKET_ACK;
    packet->client_nonce = client_nonce;
    packet->server_nonce = server_nonce;
    packet->ack = window->expected;
    packet->receive_limit = rudp_receive_window_limit(window);
    packet->sack_count = rudp_receive_window_sacks(window, packet->sacks);
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

void rudp_send_scoreboard_init(struct rudp_send_scoreboard *scoreboard, uint32_t initial_ack,
                               uint32_t receive_limit)
{
    memset(scoreboard, 0, sizeof(*scoreboard));
    scoreboard->cumulative_ack = initial_ack;
    scoreboard->receive_limit = receive_limit;
}

bool rudp_send_scoreboard_track(struct rudp_send_scoreboard *scoreboard, uint32_t sequence)
{
    struct rudp_send_slot *slot = &scoreboard->slots[sequence % RUDP_WINDOW_CAPACITY];

    if (!rudp_seq_in_window(sequence, scoreboard->cumulative_ack,
                            scoreboard->cumulative_ack + RUDP_WINDOW_CAPACITY) ||
        slot->in_use) {
        return false;
    }
    slot->sequence = sequence;
    slot->in_use = true;
    return true;
}

bool rudp_send_scoreboard_apply_ack(struct rudp_send_scoreboard *scoreboard, uint32_t ack,
                                    const struct rudp_sack_block *sacks, uint8_t sack_count,
                                    uint32_t *fast_retransmit_sequence)
{
    uint32_t sequence;
    uint8_t index;
    bool changed = false;

    if (sack_count > RUDP_MAX_SACK_BLOCKS ||
        !rudp_seq_in_window(ack, scoreboard->cumulative_ack, scoreboard->receive_limit + 1U)) {
        return false;
    }
    if (rudp_seq_after(ack, scoreboard->cumulative_ack)) {
        for (sequence = scoreboard->cumulative_ack; sequence != ack; ++sequence) {
            scoreboard->slots[sequence % RUDP_WINDOW_CAPACITY].in_use = false;
        }
        scoreboard->cumulative_ack = ack;
        changed = true;
    }
    for (index = 0U; index < sack_count; ++index) {
        for (sequence = sacks[index].start; sequence != sacks[index].end; ++sequence) {
            struct rudp_send_slot *slot = &scoreboard->slots[sequence % RUDP_WINDOW_CAPACITY];
            uint32_t lower;

            if (!slot->in_use || slot->sequence != sequence) {
                continue;
            }
            if (slot->sacked) {
                continue;
            }
            slot->sacked = true;
            changed = true;
            for (lower = scoreboard->cumulative_ack; lower != sequence; ++lower) {
                struct rudp_send_slot *hole = &scoreboard->slots[lower % RUDP_WINDOW_CAPACITY];
                if (hole->in_use && !hole->sacked && hole->sequence == lower &&
                    hole->higher_sack_evidence < 3U) {
                    hole->higher_sack_evidence += 1U;
                    if (hole->higher_sack_evidence == 3U && !hole->fast_retransmitted) {
                        hole->fast_retransmitted = true;
                        *fast_retransmit_sequence = lower;
                    }
                }
            }
        }
    }
    return changed;
}
