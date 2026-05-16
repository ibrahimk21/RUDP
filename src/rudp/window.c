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
                                const uint8_t *data, uint16_t length)
{
    struct rudp_receive_slot *slot;

    if (data == NULL || length == 0U || length > RUDP_MAX_DATA_PAYLOAD ||
        !rudp_seq_in_window(sequence, window->consumed, window->consumed + RUDP_WINDOW_CAPACITY)) {
        return false;
    }
    slot = &window->slots[sequence % RUDP_WINDOW_CAPACITY];
    if (slot->present) {
        return slot->sequence == sequence;
    }
    slot->sequence = sequence;
    slot->length = length;
    memcpy(slot->data, data, length);
    slot->present = true;
    while (window->slots[window->expected % RUDP_WINDOW_CAPACITY].present &&
           window->slots[window->expected % RUDP_WINDOW_CAPACITY].sequence == window->expected) {
        window->expected += 1U;
    }
    return true;
}

const struct rudp_receive_slot *
rudp_receive_window_next_consumable(const struct rudp_receive_window *window)
{
    const struct rudp_receive_slot *slot =
        &window->slots[window->consumed % RUDP_WINDOW_CAPACITY];

    if (!slot->present || slot->sequence != window->consumed) {
        return NULL;
    }
    return slot;
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
    scoreboard->next_sequence = initial_ack;
}

bool rudp_send_scoreboard_track(struct rudp_send_scoreboard *scoreboard, uint32_t sequence,
                                const uint8_t *data, uint16_t length)
{
    struct rudp_send_slot *slot = &scoreboard->slots[sequence % RUDP_WINDOW_CAPACITY];

    if (data == NULL || length == 0U || length > RUDP_MAX_DATA_PAYLOAD ||
        sequence != scoreboard->next_sequence ||
        !rudp_seq_in_window(sequence, scoreboard->cumulative_ack,
                            scoreboard->cumulative_ack + RUDP_WINDOW_CAPACITY) ||
        !rudp_seq_in_window(sequence, scoreboard->cumulative_ack, scoreboard->receive_limit) ||
        slot->in_use) {
        return false;
    }
    slot->sequence = sequence;
    slot->length = length;
    memcpy(slot->data, data, length);
    slot->in_use = true;
    scoreboard->next_sequence += 1U;
    return true;
}

static bool ack_in_sent_range(const struct rudp_send_scoreboard *scoreboard, uint32_t ack)
{
    const uint32_t distance = ack - scoreboard->cumulative_ack;
    const uint32_t sent_distance = scoreboard->next_sequence - scoreboard->cumulative_ack;

    return distance <= sent_distance && distance < UINT32_C(0x80000000);
}

static bool sacks_valid(const struct rudp_send_scoreboard *scoreboard, uint32_t ack,
                        const struct rudp_sack_block *sacks, uint8_t sack_count)
{
    uint8_t index;
    uint32_t previous_end = ack;

    if (sack_count > RUDP_MAX_SACK_BLOCKS || (sack_count != 0U && sacks == NULL)) {
        return false;
    }
    for (index = 0U; index < sack_count; ++index) {
        const uint32_t start_distance = sacks[index].start - ack;
        const uint32_t end_distance = sacks[index].end - ack;
        const uint32_t sent_distance = scoreboard->next_sequence - ack;

        if (start_distance == 0U || start_distance >= UINT32_C(0x80000000) ||
            end_distance <= start_distance || end_distance > sent_distance ||
            (index != 0U && !rudp_seq_after(sacks[index].start, previous_end))) {
            return false;
        }
        previous_end = sacks[index].end;
    }
    return true;
}

enum rudp_ack_result
rudp_send_scoreboard_apply_ack(struct rudp_send_scoreboard *scoreboard, uint32_t ack,
                               uint32_t receive_limit, const struct rudp_sack_block *sacks,
                               uint8_t sack_count, struct rudp_ack_update *update)
{
    uint32_t sequence;
    uint8_t index;
    struct rudp_ack_update local_update = {0};
    uint32_t advertised_width;

    if (scoreboard == NULL || update == NULL) {
        return RUDP_ACK_INVALID;
    }
    advertised_width = receive_limit - ack;
    if (!ack_in_sent_range(scoreboard, ack) ||
        advertised_width > RUDP_WINDOW_CAPACITY || advertised_width >= UINT32_C(0x80000000) ||
        !sacks_valid(scoreboard, ack, sacks, sack_count)) {
        return RUDP_ACK_INVALID;
    }
    if (rudp_seq_after(ack, scoreboard->cumulative_ack)) {
        for (sequence = scoreboard->cumulative_ack; sequence != ack; ++sequence) {
            struct rudp_send_slot *slot = &scoreboard->slots[sequence % RUDP_WINDOW_CAPACITY];

            if (slot->in_use && slot->sequence == sequence) {
                if (!slot->sacked) {
                    local_update.newly_acked += 1U;
                }
                memset(slot, 0, sizeof(*slot));
            }
        }
        scoreboard->cumulative_ack = ack;
        local_update.cumulative_advanced = true;
    }
    if (rudp_seq_after(receive_limit, scoreboard->receive_limit)) {
        scoreboard->receive_limit = receive_limit;
        local_update.credit_advanced = true;
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
            local_update.newly_sacked += 1U;
            for (lower = scoreboard->cumulative_ack; lower != sequence; ++lower) {
                struct rudp_send_slot *hole = &scoreboard->slots[lower % RUDP_WINDOW_CAPACITY];
                if (hole->in_use && !hole->sacked && hole->sequence == lower &&
                    hole->higher_sack_evidence < 3U) {
                    hole->higher_sack_evidence += 1U;
                    if (hole->higher_sack_evidence == 3U && !hole->fast_retransmitted) {
                        hole->fast_retransmitted = true;
                        hole->needs_fast_retransmit = true;
                    }
                }
            }
        }
    }
    *update = local_update;
    if (local_update.cumulative_advanced || local_update.credit_advanced ||
        local_update.newly_sacked != 0U) {
        return RUDP_ACK_CHANGED;
    }
    return RUDP_ACK_UNCHANGED;
}

size_t rudp_send_scoreboard_retained(const struct rudp_send_scoreboard *scoreboard)
{
    return (size_t)(scoreboard->next_sequence - scoreboard->cumulative_ack);
}

size_t rudp_send_scoreboard_flight(const struct rudp_send_scoreboard *scoreboard)
{
    size_t count = 0U;
    uint32_t sequence;

    for (sequence = scoreboard->cumulative_ack; sequence != scoreboard->next_sequence;
         ++sequence) {
        const struct rudp_send_slot *slot = &scoreboard->slots[sequence % RUDP_WINDOW_CAPACITY];

        if (slot->in_use && slot->sequence == sequence && !slot->sacked) {
            count += 1U;
        }
    }
    return count;
}

struct rudp_send_slot *rudp_send_scoreboard_find(struct rudp_send_scoreboard *scoreboard,
                                                 uint32_t sequence)
{
    struct rudp_send_slot *slot = &scoreboard->slots[sequence % RUDP_WINDOW_CAPACITY];

    if (!slot->in_use || slot->sequence != sequence) {
        return NULL;
    }
    return slot;
}

struct rudp_send_slot *
rudp_send_scoreboard_next_fast_retransmit(struct rudp_send_scoreboard *scoreboard)
{
    uint32_t sequence;

    for (sequence = scoreboard->cumulative_ack; sequence != scoreboard->next_sequence;
         ++sequence) {
        struct rudp_send_slot *slot = rudp_send_scoreboard_find(scoreboard, sequence);

        if (slot != NULL && slot->needs_fast_retransmit) {
            slot->needs_fast_retransmit = false;
            return slot;
        }
    }
    return NULL;
}

void rudp_send_scoreboard_mark_retransmitted(struct rudp_send_slot *slot)
{
    slot->higher_sack_evidence = 0U;
    slot->fast_retransmitted = false;
    slot->needs_fast_retransmit = false;
}
