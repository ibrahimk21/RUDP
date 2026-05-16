#ifndef RUDP_TEST_SCHEDULER_H
#define RUDP_TEST_SCHEDULER_H

#include "rudp/packet.h"
#include "rudp/session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RUDP_TEST_MAX_RULES 64U
#define RUDP_TEST_MAX_QUEUED_PACKETS 512U

enum rudp_test_direction {
    RUDP_TEST_FORWARD,
    RUDP_TEST_REVERSE,
};

enum rudp_test_action {
    RUDP_TEST_PASS,
    RUDP_TEST_DROP,
    RUDP_TEST_DELAY,
    RUDP_TEST_DUPLICATE,
    RUDP_TEST_CORRUPT,
};

struct rudp_test_rule {
    enum rudp_test_direction direction;
    enum rudp_packet_type type;
    uint32_t sequence;
    uint32_t occurrence;
    enum rudp_test_action action;
    uint64_t delay_ms;
    uint32_t seen;
};

struct rudp_test_datagram {
    struct rudp_packet packet;
    uint8_t payload[RUDP_MAX_DATA_PAYLOAD];
    enum rudp_test_direction direction;
    uint64_t due_ms;
    uint64_t order;
    bool corrupted;
    bool occupied;
};

struct rudp_test_scheduler {
    uint64_t now_ms;
    uint64_t next_order;
    uint32_t random_state[2];
    struct rudp_test_rule rules[RUDP_TEST_MAX_RULES];
    size_t rule_count;
    struct rudp_test_datagram queue[RUDP_TEST_MAX_QUEUED_PACKETS];
    size_t queued_count;
};

struct rudp_test_link {
    struct rudp_test_scheduler *scheduler;
    enum rudp_test_direction direction;
};

typedef void (*rudp_test_deliver_fn)(void *context, enum rudp_test_direction direction,
                                     const struct rudp_packet *packet, bool corrupted);

void rudp_test_scheduler_init(struct rudp_test_scheduler *scheduler, uint32_t forward_seed,
                              uint32_t reverse_seed);
bool rudp_test_scheduler_add_rule(struct rudp_test_scheduler *scheduler,
                                  const struct rudp_test_rule *rule);
uint32_t rudp_test_scheduler_random(struct rudp_test_scheduler *scheduler,
                                    enum rudp_test_direction direction);
uint64_t rudp_test_scheduler_now(void *context);
int rudp_test_scheduler_send(void *context, const struct rudp_peer *peer,
                             const struct rudp_packet *packet);
void rudp_test_scheduler_advance(struct rudp_test_scheduler *scheduler, uint64_t now_ms,
                                 rudp_test_deliver_fn deliver, void *context);

#endif
