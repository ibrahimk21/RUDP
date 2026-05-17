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
    scheduler->loss_accumulator[RUDP_TEST_FORWARD] = forward_seed % 100U;
    scheduler->loss_accumulator[RUDP_TEST_REVERSE] = reverse_seed % 100U;
}

void rudp_test_scheduler_set_impairment(struct rudp_test_scheduler *scheduler,
                                        const struct rudp_test_impairment *impairment)
{
    scheduler->impairment = *impairment;
}

uint64_t rudp_test_profile_one_way_ms(enum rudp_test_profile profile)
{
    static const uint64_t delays[] = {0U, 25U, 300U, 20U};

    return (unsigned int)profile < sizeof(delays) / sizeof(delays[0]) ? delays[profile] : 0U;
}

const char *rudp_test_profile_name(enum rudp_test_profile profile)
{
    static const char *const names[] = {"none", "terrestrial", "geo", "leo"};

    return (unsigned int)profile < sizeof(names) / sizeof(names[0]) ? names[profile] : "invalid";
}

const char *rudp_test_mode_name(enum rudp_test_mode mode)
{
    static const char *const names[] = {"none", "reorder", "duplicate", "combined"};

    return (unsigned int)mode < sizeof(names) / sizeof(names[0]) ? names[mode] : "invalid";
}

bool rudp_test_scheduler_add_rule(struct rudp_test_scheduler *scheduler,
                                  const struct rudp_test_rule *rule)
{
    if (scheduler == NULL || rule == NULL || scheduler->rule_count == RUDP_TEST_MAX_RULES) {
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

static bool random_percent(struct rudp_test_scheduler *scheduler,
                           enum rudp_test_direction direction, unsigned int percent)
{
    return rudp_test_scheduler_random(scheduler, direction) % 100U < percent;
}

static bool scheduled_percent(struct rudp_test_scheduler *scheduler,
                              enum rudp_test_direction direction, unsigned int percent)
{
    unsigned int value;

    if (percent == 100U) {
        return true;
    }
    value = scheduler->loss_accumulator[direction] + percent;
    scheduler->loss_accumulator[direction] = value % 100U;
    return value >= 100U;
}

static bool native_profile_drop(struct rudp_test_scheduler *scheduler,
                                enum rudp_test_direction direction)
{
    switch (scheduler->impairment.profile) {
    case RUDP_TEST_PROFILE_TERRESTRIAL:
        return rudp_test_scheduler_random(scheduler, direction) % 1000U == 0U;
    case RUDP_TEST_PROFILE_GEO:
        return rudp_test_scheduler_random(scheduler, direction) % 100U < 2U;
    case RUDP_TEST_PROFILE_LEO:
        if (scheduler->leo_bad_state[direction]) {
            if (random_percent(scheduler, direction, 20U)) {
                scheduler->leo_bad_state[direction] = false;
            }
        } else if (random_percent(scheduler, direction, 1U)) {
            scheduler->leo_bad_state[direction] = true;
        }
        return scheduler->leo_bad_state[direction];
    case RUDP_TEST_PROFILE_NONE:
        return false;
    }
    return false;
}

static void record_trace(struct rudp_test_scheduler *scheduler,
                         const struct rudp_test_trace_event *event)
{
    if (scheduler->trace_count < RUDP_TEST_TRACE_CAPACITY) {
        scheduler->trace[scheduler->trace_count++] = *event;
    } else {
        scheduler->trace_truncated = true;
    }
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
    uint64_t delay_ms = rudp_test_profile_one_way_ms(scheduler->impairment.profile);
    uint64_t number = ++scheduler->original_datagrams[link->direction];
    bool reordered = scheduler->impairment.mode == RUDP_TEST_MODE_REORDER ||
                     scheduler->impairment.mode == RUDP_TEST_MODE_COMBINED;
    bool duplicated = scheduler->impairment.mode == RUDP_TEST_MODE_DUPLICATE ||
                      scheduler->impairment.mode == RUDP_TEST_MODE_COMBINED;
    bool dropped;
    struct rudp_test_trace_event event;
    size_t index;

    (void)peer;
    for (index = 0U; index < scheduler->rule_count; ++index) {
        struct rudp_test_rule *rule = &scheduler->rules[index];

        if (!rule_matches(rule, link->direction, packet)) {
            continue;
        }
        rule->seen += 1U;
        if (rule->occurrence == 0U || rule->seen == rule->occurrence) {
            action = rule->action;
            delay_ms = rule->delay_ms;
            break;
        }
    }
    if (action == RUDP_TEST_PASS) {
        if (reordered && number % 7U == 0U) {
            delay_ms += 4U * rudp_test_profile_one_way_ms(scheduler->impairment.profile);
        }
        duplicated = duplicated && number % 11U == 0U;
    } else {
        reordered = false;
        duplicated = action == RUDP_TEST_DUPLICATE;
    }
    dropped = action == RUDP_TEST_DROP;
    if (action == RUDP_TEST_PASS) {
        dropped = scheduler->impairment.random_loss_percent >= 0
                      ? scheduled_percent(scheduler, link->direction,
                                          (unsigned int)scheduler->impairment.random_loss_percent)
                      : native_profile_drop(scheduler, link->direction);
    }
    event = (struct rudp_test_trace_event){
        .number = number,
        .at_ms = scheduler->now_ms,
        .due_ms = scheduler->now_ms + delay_ms,
        .direction = link->direction,
        .type = packet->type,
        .sequence = packet->seq,
        .dropped = dropped,
        .duplicated = duplicated,
        .reordered = reordered && number % 7U == 0U,
    };
    record_trace(scheduler, &event);
    if (dropped) {
        return 0;
    }
    if (!enqueue(scheduler, link->direction, packet, scheduler->now_ms + delay_ms,
                 action == RUDP_TEST_CORRUPT)) {
        return -1;
    }
    if (duplicated && !enqueue(scheduler, link->direction, packet,
                               scheduler->now_ms + delay_ms +
                                   rudp_test_profile_one_way_ms(scheduler->impairment.profile),
                               false)) {
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
