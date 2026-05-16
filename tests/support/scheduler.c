#include "scheduler.h"

#include <assert.h>
#include <string.h>

static bool rule_matches(const struct rudp_test_rule *rule, enum rudp_test_direction direction,
                         const struct rudp_packet *packet)
{
    return rule->direction == direction && rule->type == packet->type &&
           rule->sequence == packet->seq;
}

static bool enqueue(struct rudp_test_scheduler *scheduler, enum rudp_test_direction direction,
                    const struct rudp_packet *packet, uint64_t due_ms, bool corrupted)
{
    size_t index;

    if (scheduler->queued_count == RUDP_TEST_MAX_QUEUED_PACKETS) {
        return false;
    }
    for (index = 0U; index < RUDP_TEST_MAX_QUEUED_PACKETS; ++index) {
        struct rudp_test_datagram *datagram = &scheduler->queue[index];

        if (datagram->occupied) {
            continue;
        }
        memset(datagram, 0, sizeof(*datagram));
        datagram->packet = *packet;
        if (packet->type == RUDP_PACKET_DATA) {
            memcpy(datagram->payload, packet->data, packet->data_length);
            datagram->packet.data = datagram->payload;
        }
        datagram->direction = direction;
        datagram->due_ms = due_ms;
        datagram->order = scheduler->next_order++;
        datagram->corrupted = corrupted;
        datagram->occupied = true;
        scheduler->queued_count += 1U;
        return true;
    }
    return false;
}

void rudp_test_scheduler_init(struct rudp_test_scheduler *scheduler, uint32_t forward_seed,
                              uint32_t reverse_seed)
{
    memset(scheduler, 0, sizeof(*scheduler));
    scheduler->random_state[RUDP_TEST_FORWARD] = forward_seed == 0U ? 1U : forward_seed;
    scheduler->random_state[RUDP_TEST_REVERSE] = reverse_seed == 0U ? 1U : reverse_seed;
}

bool rudp_test_scheduler_add_rule(struct rudp_test_scheduler *scheduler,
                                  const struct rudp_test_rule *rule)
{
    if (scheduler == NULL || rule == NULL || rule->occurrence == 0U ||
        scheduler->rule_count == RUDP_TEST_MAX_RULES) {
        return false;
    }
    scheduler->rules[scheduler->rule_count++] = *rule;
    return true;
}

uint32_t rudp_test_scheduler_random(struct rudp_test_scheduler *scheduler,
                                    enum rudp_test_direction direction)
{
    uint32_t state = scheduler->random_state[direction];

    /* xorshift32 is fixed here so traces do not depend on the platform C library. */
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    scheduler->random_state[direction] = state;
    return state;
}

uint64_t rudp_test_scheduler_now(void *context)
{
    return ((struct rudp_test_scheduler *)context)->now_ms;
}

int rudp_test_scheduler_send(void *context, const struct rudp_peer *peer,
                             const struct rudp_packet *packet)
{
    struct rudp_test_link *link = context;
    struct rudp_test_scheduler *scheduler = link->scheduler;
    enum rudp_test_action action = RUDP_TEST_PASS;
    uint64_t delay_ms = 0U;
    size_t index;

    (void)peer;
    for (index = 0U; index < scheduler->rule_count; ++index) {
        struct rudp_test_rule *rule = &scheduler->rules[index];

        if (!rule_matches(rule, link->direction, packet)) {
            continue;
        }
        rule->seen += 1U;
        if (rule->seen == rule->occurrence) {
            action = rule->action;
            delay_ms = rule->delay_ms;
            break;
        }
    }
    if (action == RUDP_TEST_DROP) {
        return 0;
    }
    if (!enqueue(scheduler, link->direction, packet, scheduler->now_ms + delay_ms,
                 action == RUDP_TEST_CORRUPT)) {
        return -1;
    }
    if (action == RUDP_TEST_DUPLICATE &&
        !enqueue(scheduler, link->direction, packet, scheduler->now_ms + delay_ms + 1U, false)) {
        return -1;
    }
    return 0;
}

void rudp_test_scheduler_advance(struct rudp_test_scheduler *scheduler, uint64_t now_ms,
                                 rudp_test_deliver_fn deliver, void *context)
{
    assert(now_ms >= scheduler->now_ms);
    scheduler->now_ms = now_ms;
    for (;;) {
        struct rudp_test_datagram *next = NULL;
        size_t index;

        for (index = 0U; index < RUDP_TEST_MAX_QUEUED_PACKETS; ++index) {
            struct rudp_test_datagram *candidate = &scheduler->queue[index];

            if (!candidate->occupied || candidate->due_ms > now_ms) {
                continue;
            }
            if (next == NULL || candidate->due_ms < next->due_ms ||
                (candidate->due_ms == next->due_ms && candidate->order < next->order)) {
                next = candidate;
            }
        }
        if (next == NULL) {
            return;
        }
        deliver(context, next->direction, &next->packet, next->corrupted);
        next->occupied = false;
        scheduler->queued_count -= 1U;
    }
}
