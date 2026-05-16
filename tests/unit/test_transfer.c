#include "rudp/transfer.h"
#include "scheduler.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct dynamic_sink {
    uint8_t *bytes;
    size_t capacity;
    size_t length;
    bool finished;
};

struct transfer_pair {
    struct rudp_test_scheduler *scheduler;
    struct rudp_windowed_sender *sender;
    struct rudp_windowed_receiver *receiver;
    struct rudp_test_link sender_link;
    struct rudp_test_link receiver_link;
    struct dynamic_sink sink;
    uint8_t *source;
    size_t source_length;
    uint32_t fixed_window;
    bool consume_immediately;
};

struct capture_context {
    uint64_t now_ms;
    struct rudp_packet packets[32];
    uint8_t payloads[32][RUDP_MAX_DATA_PAYLOAD];
    size_t count;
};

static int sink_append(void *context, const uint8_t *bytes, size_t length)
{
    struct dynamic_sink *sink = context;

    if (sink->length + length > sink->capacity) {
        return -1;
    }
    memcpy(sink->bytes + sink->length, bytes, length);
    sink->length += length;
    return 0;
}

static int sink_finish(void *context, uint64_t length, const uint8_t digest[16])
{
    struct dynamic_sink *sink = context;

    (void)digest;
    if (length != sink->length) {
        return -1;
    }
    sink->finished = true;
    return 0;
}

static void transfer_deliver(void *context, enum rudp_test_direction direction,
                             const struct rudp_packet *packet, bool corrupted)
{
    struct transfer_pair *pair = context;
    enum rudp_transfer_error error;

    if (corrupted) {
        return;
    }
    if (direction == RUDP_TEST_FORWARD) {
        size_t consumed;

        error = rudp_windowed_receiver_receive(pair->receiver, packet);
        assert(error == RUDP_TRANSFER_OK);
        if (pair->consume_immediately && packet->type == RUDP_PACKET_DATA) {
            assert(rudp_windowed_receiver_consume(pair->receiver, RUDP_WINDOW_CAPACITY,
                                                  &consumed) == RUDP_TRANSFER_OK);
        }
    } else {
        error = rudp_windowed_sender_receive(pair->sender, packet);
        assert(error == RUDP_TRANSFER_OK);
    }
}

static struct transfer_pair *transfer_pair_create(size_t source_length, uint32_t fixed_window)
{
    struct transfer_pair *pair = calloc(1U, sizeof(*pair));
    size_t index;

    assert(pair != NULL);
    pair->scheduler = calloc(1U, sizeof(*pair->scheduler));
    pair->sender = calloc(1U, sizeof(*pair->sender));
    pair->receiver = calloc(1U, sizeof(*pair->receiver));
    pair->source = malloc(source_length == 0U ? 1U : source_length);
    pair->sink.bytes = malloc(source_length == 0U ? 1U : source_length);
    assert(pair->scheduler != NULL && pair->sender != NULL && pair->receiver != NULL &&
           pair->source != NULL && pair->sink.bytes != NULL);
    pair->source_length = source_length;
    pair->fixed_window = fixed_window;
    pair->sink.capacity = source_length;
    pair->consume_immediately = true;
    for (index = 0U; index < source_length; ++index) {
        pair->source[index] = (uint8_t)(index * 17U);
    }
    rudp_test_scheduler_init(pair->scheduler, UINT32_C(0x12345678), UINT32_C(0x87654321));
    pair->sender_link = (struct rudp_test_link){pair->scheduler, RUDP_TEST_FORWARD};
    pair->receiver_link = (struct rudp_test_link){pair->scheduler, RUDP_TEST_REVERSE};
    return pair;
}

static void transfer_pair_start(struct transfer_pair *pair)
{
    static const uint8_t digest[16] = {1U};
    const struct rudp_clock clock = {pair->scheduler, rudp_test_scheduler_now};
    const struct rudp_session_io sender_io = {&pair->sender_link, rudp_test_scheduler_send};
    const struct rudp_session_io receiver_io = {&pair->receiver_link, rudp_test_scheduler_send};
    const struct rudp_peer peer = {.ipv4_address = UINT32_C(0x7f000001), .port = 1U};
    const struct rudp_transfer_metadata metadata = {
        .length = pair->source_length,
        .digest = {1U},
    };
    const struct rudp_transfer_sink sink_api = {
        &pair->sink,
        sink_append,
        sink_finish,
    };

    assert(rudp_windowed_receiver_start(pair->receiver, &clock, &receiver_io, &peer, 11U, 22U,
                                        &metadata, &sink_api) == RUDP_TRANSFER_OK);
    assert(rudp_windowed_sender_start(pair->sender, &clock, &sender_io, &peer, 11U, 22U,
                                      pair->source, pair->source_length, digest, pair->fixed_window,
                                      RUDP_WINDOW_CAPACITY) == RUDP_TRANSFER_OK);
}

static void transfer_pair_destroy(struct transfer_pair *pair)
{
    free(pair->sink.bytes);
    free(pair->source);
    free(pair->receiver);
    free(pair->sender);
    free(pair->scheduler);
    free(pair);
}

static void transfer_pair_run(struct transfer_pair *pair, uint64_t deadline_ms)
{
    uint64_t now;

    for (now = pair->scheduler->now_ms; now <= deadline_ms; ++now) {
        rudp_test_scheduler_advance(pair->scheduler, now, transfer_deliver, pair);
        (void)rudp_windowed_sender_tick(pair->sender);
        (void)rudp_windowed_receiver_tick(pair->receiver);
        rudp_test_scheduler_advance(pair->scheduler, now, transfer_deliver, pair);
        if (pair->sender->state == RUDP_TRANSFER_COMPLETE ||
            pair->sender->state == RUDP_TRANSFER_FAILED) {
            return;
        }
    }
}

static uint64_t capture_now(void *context)
{
    return ((struct capture_context *)context)->now_ms;
}

static int capture_send(void *context, const struct rudp_peer *peer,
                        const struct rudp_packet *packet)
{
    struct capture_context *capture = context;

    (void)peer;
    assert(capture->count < sizeof(capture->packets) / sizeof(capture->packets[0]));
    capture->packets[capture->count] = *packet;
    if (packet->type == RUDP_PACKET_DATA) {
        memcpy(capture->payloads[capture->count], packet->data, packet->data_length);
        capture->packets[capture->count].data = capture->payloads[capture->count];
    }
    capture->count += 1U;
    return 0;
}

static void test_windowed_recovery(void)
{
    const struct rudp_test_rule rules[] = {
        {RUDP_TEST_FORWARD, RUDP_PACKET_DATA, 1U, 1U, RUDP_TEST_DELAY, 50U, 0U},
        {RUDP_TEST_FORWARD, RUDP_PACKET_DATA, 2U, 1U, RUDP_TEST_DROP, 0U, 0U},
        {RUDP_TEST_FORWARD, RUDP_PACKET_DATA, 4U, 1U, RUDP_TEST_DUPLICATE, 0U, 0U},
    };
    struct transfer_pair *pair = transfer_pair_create(10U * 1024U + 17U, 8U);
    size_t index;

    for (index = 0U; index < sizeof(rules) / sizeof(rules[0]); ++index) {
        assert(rudp_test_scheduler_add_rule(pair->scheduler, &rules[index]));
    }
    transfer_pair_start(pair);
    transfer_pair_run(pair, 5000U);
    assert(pair->sender->state == RUDP_TRANSFER_COMPLETE);
    assert(pair->sender->fast_retransmits >= 2U);
    assert(pair->sink.finished);
    assert(pair->sink.length == pair->source_length);
    assert(memcmp(pair->sink.bytes, pair->source, pair->source_length) == 0);
    transfer_pair_destroy(pair);
}

static void test_credit_probe_and_lost_update(void)
{
    static const uint8_t digest[16] = {1U};
    uint8_t source[2048] = {0};
    struct capture_context capture = {0};
    struct rudp_windowed_sender *sender = calloc(1U, sizeof(*sender));
    const struct rudp_clock clock = {&capture, capture_now};
    const struct rudp_session_io io = {&capture, capture_send};
    const struct rudp_peer peer = {.ipv4_address = UINT32_C(0x7f000001), .port = 1U};
    struct rudp_packet ack = {
        .type = RUDP_PACKET_ACK,
        .client_nonce = 11U,
        .server_nonce = 22U,
        .ack = 1U,
        .receive_limit = 1U,
    };

    assert(sender != NULL);
    assert(rudp_windowed_sender_start(sender, &clock, &io, &peer, 11U, 22U, source, sizeof(source),
                                      digest, 4U, 1U) == RUDP_TRANSFER_OK);
    assert(capture.count == 1U && capture.packets[0].type == RUDP_PACKET_DATA);
    assert(rudp_windowed_sender_receive(sender, &ack) == RUDP_TRANSFER_OK);
    assert(sender->probe_timer_ms == 1000U);
    capture.now_ms = 1000U;
    assert(rudp_windowed_sender_tick(sender) == RUDP_TRANSFER_OK);
    assert(capture.packets[capture.count - 1U].type == RUDP_PACKET_PROBE);
    ack.receive_limit = 2U;
    assert(rudp_windowed_sender_receive(sender, &ack) == RUDP_TRANSFER_OK);
    assert(capture.packets[capture.count - 1U].type == RUDP_PACKET_DATA);
    assert(capture.packets[capture.count - 1U].seq == 1U);
    free(sender);
}

static void test_receiver_answers_probe(void)
{
    struct capture_context capture = {0};
    struct dynamic_sink sink = {0};
    struct rudp_windowed_receiver *receiver = calloc(1U, sizeof(*receiver));
    const struct rudp_clock clock = {&capture, capture_now};
    const struct rudp_session_io io = {&capture, capture_send};
    const struct rudp_peer peer = {.ipv4_address = UINT32_C(0x7f000001), .port = 1U};
    const struct rudp_transfer_metadata metadata = {.length = 1U, .digest = {1U}};
    const struct rudp_transfer_sink sink_api = {&sink, sink_append, sink_finish};
    const struct rudp_packet probe = {
        .type = RUDP_PACKET_PROBE,
        .client_nonce = 11U,
        .server_nonce = 22U,
    };

    assert(receiver != NULL);
    assert(rudp_windowed_receiver_start(receiver, &clock, &io, &peer, 11U, 22U, &metadata,
                                        &sink_api) == RUDP_TRANSFER_OK);
    assert(rudp_windowed_receiver_receive(receiver, &probe) == RUDP_TRANSFER_OK);
    assert(capture.count == 1U);
    assert(capture.packets[0].type == RUDP_PACKET_ACK);
    assert(capture.packets[0].ack == 0U);
    assert(capture.packets[0].receive_limit == RUDP_WINDOW_CAPACITY);
    free(receiver);
}

static void test_stopped_consumer_and_sack_limit(void)
{
    static const uint8_t byte = 7U;
    struct rudp_receive_window *window = calloc(1U, sizeof(*window));
    struct rudp_sack_block sacks[RUDP_MAX_SACK_BLOCKS];
    uint32_t sequence;

    assert(window != NULL);
    rudp_receive_window_init(window, 0U);
    for (sequence = 0U; sequence < RUDP_WINDOW_CAPACITY; ++sequence) {
        assert(rudp_receive_window_insert(window, sequence, &byte, 1U));
    }
    assert(window->expected == RUDP_WINDOW_CAPACITY);
    assert(window->consumed == 0U);
    assert(rudp_receive_window_limit(window) == RUDP_WINDOW_CAPACITY);
    assert(!rudp_receive_window_insert(window, RUDP_WINDOW_CAPACITY, &byte, 1U));
    assert(rudp_receive_window_consume(window, 0U));
    assert(rudp_receive_window_limit(window) == RUDP_WINDOW_CAPACITY + 1U);

    rudp_receive_window_init(window, 0U);
    for (sequence = 1U; sequence <= 11U; sequence += 2U) {
        assert(rudp_receive_window_insert(window, sequence, &byte, 1U));
    }
    assert(rudp_receive_window_sacks(window, sacks) == RUDP_MAX_SACK_BLOCKS);
    assert(sacks[0].start == 1U && sacks[0].end == 2U);
    assert(sacks[3].start == 7U && sacks[3].end == 8U);
    free(window);
}

static void track_range(struct rudp_send_scoreboard *scoreboard, uint32_t count)
{
    static const uint8_t byte = 1U;
    uint32_t sequence;

    for (sequence = 0U; sequence < count; ++sequence) {
        assert(rudp_send_scoreboard_track(scoreboard, sequence, &byte, 1U));
    }
}

static void test_ack_validation_and_recovery_suppression(void)
{
    struct rudp_send_scoreboard *scoreboard = calloc(1U, sizeof(*scoreboard));
    struct rudp_ack_update update;
    const struct rudp_sack_block first_three = {1U, 4U};
    const struct rudp_sack_block next_three = {4U, 7U};
    struct rudp_send_slot *slot;

    assert(scoreboard != NULL);
    rudp_send_scoreboard_init(scoreboard, 0U, 8U);
    track_range(scoreboard, 8U);
    assert(rudp_send_scoreboard_apply_ack(scoreboard, 9U, 9U, NULL, 0U, &update) ==
           RUDP_ACK_INVALID);
    assert(rudp_send_scoreboard_apply_ack(scoreboard, 0U, 7U, NULL, 0U, &update) ==
           RUDP_ACK_UNCHANGED);
    assert(scoreboard->receive_limit == 8U);
    assert(rudp_send_scoreboard_apply_ack(scoreboard, 0U, 8U, &first_three, 1U, &update) ==
           RUDP_ACK_CHANGED);
    slot = rudp_send_scoreboard_next_fast_retransmit(scoreboard);
    assert(slot != NULL && slot->sequence == 0U);
    rudp_send_scoreboard_mark_retransmitted(slot);
    assert(rudp_send_scoreboard_apply_ack(scoreboard, 0U, 8U, &first_three, 1U, &update) ==
           RUDP_ACK_UNCHANGED);
    assert(rudp_send_scoreboard_next_fast_retransmit(scoreboard) == NULL);
    assert(rudp_send_scoreboard_apply_ack(scoreboard, 0U, 8U, &next_three, 1U, &update) ==
           RUDP_ACK_CHANGED);
    slot = rudp_send_scoreboard_next_fast_retransmit(scoreboard);
    assert(slot != NULL && slot->sequence == 0U);
    rudp_send_scoreboard_mark_retransmitted(slot);
    assert(rudp_send_scoreboard_next_fast_retransmit(scoreboard) == NULL);
    assert(rudp_send_scoreboard_retained(scoreboard) == 8U);
    assert(rudp_send_scoreboard_flight(scoreboard) == 2U);
    assert(rudp_send_scoreboard_apply_ack(scoreboard, 1U, 8U, NULL, 0U, &update) ==
           RUDP_ACK_CHANGED);
    assert(rudp_send_scoreboard_apply_ack(scoreboard, 0U, 8U, NULL, 0U, &update) ==
           RUDP_ACK_INVALID);
    free(scoreboard);
}

static void test_tail_loss_uses_timer(void)
{
    const struct rudp_test_rule drop_tail = {
        RUDP_TEST_FORWARD, RUDP_PACKET_DATA, 0U, 0U, RUDP_TEST_DROP, 0U, 0U,
    };
    struct transfer_pair *pair = transfer_pair_create(1U, 4U);

    assert(rudp_test_scheduler_add_rule(pair->scheduler, &drop_tail));
    transfer_pair_start(pair);
    transfer_pair_run(pair, 1000U);
    assert(pair->sender->state == RUDP_TRANSFER_SENDING);
    assert(pair->sender->fast_retransmits == 0U);
    assert(pair->sender->timeout_retransmits == 1U);
    transfer_pair_destroy(pair);
}

static void test_memory_caps(void)
{
    assert(sizeof(struct rudp_receive_window) <=
           RUDP_WINDOW_CAPACITY * (RUDP_MAX_DATA_PAYLOAD + 16U));
    assert(sizeof(struct rudp_send_scoreboard) <=
           RUDP_WINDOW_CAPACITY * (RUDP_MAX_DATA_PAYLOAD + 32U));
}

int main(void)
{
    test_windowed_recovery();
    test_credit_probe_and_lost_update();
    test_receiver_answers_probe();
    test_stopped_consumer_and_sack_limit();
    test_ack_validation_and_recovery_suppression();
    test_tail_loss_uses_timer();
    test_memory_caps();
    puts("windowed transfer tests passed");
    return 0;
}
