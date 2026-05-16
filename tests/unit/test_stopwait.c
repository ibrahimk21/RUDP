#include "rudp/stopwait.h"
#include "scheduler.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fake_context {
    uint64_t now;
    struct rudp_packet sent[16];
    size_t sent_count;
};

struct memory_sink {
    uint8_t bytes[2048];
    size_t length;
    int finished;
};

struct scheduled_pair {
    struct rudp_test_scheduler scheduler;
    struct rudp_test_link sender_link;
    struct rudp_test_link receiver_link;
    struct rudp_stopwait_sender sender;
    struct rudp_stopwait_receiver receiver;
    struct memory_sink sink;
    uint8_t source[1025];
};

static uint64_t fake_now(void *context)
{
    return ((struct fake_context *)context)->now;
}

static int fake_send(void *context, const struct rudp_peer *peer, const struct rudp_packet *packet)
{
    struct fake_context *fake = context;

    (void)peer;
    assert(fake->sent_count < sizeof(fake->sent) / sizeof(fake->sent[0]));
    fake->sent[fake->sent_count++] = *packet;
    return 0;
}

static int sink_append(void *context, const uint8_t *bytes, size_t length)
{
    struct memory_sink *sink = context;

    assert(sink->length + length <= sizeof(sink->bytes));
    memcpy(sink->bytes + sink->length, bytes, length);
    sink->length += length;
    return 0;
}

static int sink_finish(void *context, uint64_t length, const uint8_t digest[16])
{
    struct memory_sink *sink = context;

    (void)digest;
    assert(length == sink->length);
    sink->finished = 1;
    return 0;
}

static void start_pair(struct rudp_stopwait_sender *sender, struct rudp_stopwait_receiver *receiver,
                       struct fake_context *sender_context, struct fake_context *receiver_context,
                       struct memory_sink *sink, const uint8_t *source, size_t length)
{
    static const uint8_t digest[16] = {1U};
    const struct rudp_clock sender_clock = {.context = sender_context, .now_ms = fake_now};
    const struct rudp_clock receiver_clock = {.context = receiver_context, .now_ms = fake_now};
    const struct rudp_session_io sender_io = {.context = sender_context, .send = fake_send};
    const struct rudp_session_io receiver_io = {.context = receiver_context, .send = fake_send};
    const struct rudp_peer peer = {.ipv4_address = UINT32_C(0x7f000001), .port = 1U};
    const struct rudp_transfer_metadata metadata = {.length = length, .digest = {1U}};
    const struct rudp_transfer_sink sink_api = {
        .context = sink,
        .append = sink_append,
        .finish = sink_finish,
    };

    assert(rudp_stopwait_receiver_start(receiver, &receiver_clock, &receiver_io, &peer, 11U, 22U,
                                        &metadata, &sink_api) == RUDP_TRANSFER_OK);
    assert(rudp_stopwait_sender_start(sender, &sender_clock, &sender_io, &peer, 11U, 22U, source,
                                      length, digest) == RUDP_TRANSFER_OK);
}

static void scheduled_deliver(void *context, enum rudp_test_direction direction,
                              const struct rudp_packet *packet, bool corrupted)
{
    struct scheduled_pair *pair = context;
    enum rudp_transfer_error error;

    if (corrupted) {
        return;
    }
    if (direction == RUDP_TEST_FORWARD) {
        error = rudp_stopwait_receiver_receive(&pair->receiver, packet);
    } else {
        error = rudp_stopwait_sender_receive(&pair->sender, packet);
    }
    assert(error == RUDP_TRANSFER_OK);
}

static void scheduled_pair_start(struct scheduled_pair *pair)
{
    static const uint8_t digest[16] = {1U};
    struct rudp_clock clock;
    struct rudp_session_io sender_io;
    struct rudp_session_io receiver_io;
    const struct rudp_peer peer = {.ipv4_address = UINT32_C(0x7f000001), .port = 1U};
    const struct rudp_transfer_metadata metadata = {
        .length = sizeof(pair->source),
        .digest = {1U},
    };
    const struct rudp_transfer_sink sink_api = {
        .context = &pair->sink,
        .append = sink_append,
        .finish = sink_finish,
    };
    size_t index;

    for (index = 0U; index < sizeof(pair->source); ++index) {
        pair->source[index] = (uint8_t)index;
    }
    pair->sender_link = (struct rudp_test_link){
        .scheduler = &pair->scheduler,
        .direction = RUDP_TEST_FORWARD,
    };
    pair->receiver_link = (struct rudp_test_link){
        .scheduler = &pair->scheduler,
        .direction = RUDP_TEST_REVERSE,
    };
    clock = (struct rudp_clock){
        .context = &pair->scheduler,
        .now_ms = rudp_test_scheduler_now,
    };
    sender_io = (struct rudp_session_io){
        .context = &pair->sender_link,
        .send = rudp_test_scheduler_send,
    };
    receiver_io = (struct rudp_session_io){
        .context = &pair->receiver_link,
        .send = rudp_test_scheduler_send,
    };
    assert(rudp_stopwait_receiver_start(&pair->receiver, &clock, &receiver_io, &peer, 11U, 22U,
                                        &metadata, &sink_api) == RUDP_TRANSFER_OK);
    assert(rudp_stopwait_sender_start(&pair->sender, &clock, &sender_io, &peer, 11U, 22U,
                                      pair->source, sizeof(pair->source),
                                      digest) == RUDP_TRANSFER_OK);
}

static void scheduled_run(struct scheduled_pair *pair, uint64_t deadline_ms)
{
    uint64_t now;

    for (now = 0U; now <= deadline_ms; ++now) {
        rudp_test_scheduler_advance(&pair->scheduler, now, scheduled_deliver, pair);
        (void)rudp_stopwait_sender_tick(&pair->sender);
        (void)rudp_stopwait_receiver_tick(&pair->receiver);
        rudp_test_scheduler_advance(&pair->scheduler, now, scheduled_deliver, pair);
        if (pair->sender.state == RUDP_TRANSFER_COMPLETE ||
            pair->sender.state == RUDP_TRANSFER_FAILED) {
            return;
        }
    }
}

static void run_success_rule(const struct rudp_test_rule *rule)
{
    struct scheduled_pair pair;

    memset(&pair, 0, sizeof(pair));
    rudp_test_scheduler_init(&pair.scheduler, 1U, 2U);
    assert(rudp_test_scheduler_add_rule(&pair.scheduler, rule));
    scheduled_pair_start(&pair);
    scheduled_run(&pair, 5000U);
    assert(pair.sender.state == RUDP_TRANSFER_COMPLETE);
    assert(pair.sink.finished == 1);
    assert(pair.sink.length == sizeof(pair.source));
    assert(memcmp(pair.sink.bytes, pair.source, sizeof(pair.source)) == 0);
}

static void test_scripted_impairments(void)
{
    const struct rudp_test_rule rules[] = {
        {RUDP_TEST_FORWARD, RUDP_PACKET_DATA, 0U, 1U, RUDP_TEST_DROP, 0U, 0U},
        {RUDP_TEST_REVERSE, RUDP_PACKET_ACK, 0U, 1U, RUDP_TEST_DROP, 0U, 0U},
        {RUDP_TEST_FORWARD, RUDP_PACKET_FIN, 2U, 1U, RUDP_TEST_DROP, 0U, 0U},
        {RUDP_TEST_REVERSE, RUDP_PACKET_FIN_ACK, 2U, 1U, RUDP_TEST_DROP, 0U, 0U},
        {RUDP_TEST_FORWARD, RUDP_PACKET_DATA, 0U, 1U, RUDP_TEST_DUPLICATE, 0U, 0U},
        {RUDP_TEST_FORWARD, RUDP_PACKET_DATA, 0U, 1U, RUDP_TEST_DELAY, 25U, 0U},
        {RUDP_TEST_FORWARD, RUDP_PACKET_DATA, 0U, 1U, RUDP_TEST_CORRUPT, 0U, 0U},
    };
    size_t index;

    for (index = 0U; index < sizeof(rules) / sizeof(rules[0]); ++index) {
        run_success_rule(&rules[index]);
    }
}

static void test_directional_randomness(void)
{
    struct rudp_test_scheduler scheduler;

    rudp_test_scheduler_init(&scheduler, 1U, 2U);
    assert(rudp_test_scheduler_random(&scheduler, RUDP_TEST_FORWARD) == UINT32_C(270369));
    assert(rudp_test_scheduler_random(&scheduler, RUDP_TEST_REVERSE) == UINT32_C(540738));
    assert(rudp_test_scheduler_random(&scheduler, RUDP_TEST_FORWARD) == UINT32_C(67634689));
}

static void test_peer_death_and_completion_unknown(void)
{
    struct scheduled_pair pair;
    const struct rudp_test_rule drop_data = {
        RUDP_TEST_FORWARD, RUDP_PACKET_DATA, 0U, 0U, RUDP_TEST_DROP, 0U, 0U,
    };
    const struct rudp_test_rule drop_fin_ack = {
        RUDP_TEST_REVERSE, RUDP_PACKET_FIN_ACK, 2U, 0U, RUDP_TEST_DROP, 0U, 0U,
    };

    memset(&pair, 0, sizeof(pair));
    rudp_test_scheduler_init(&pair.scheduler, 1U, 2U);
    assert(rudp_test_scheduler_add_rule(&pair.scheduler, &drop_data));
    scheduled_pair_start(&pair);
    scheduled_run(&pair, RUDP_DATA_PROGRESS_TIMEOUT_MS);
    assert(pair.sender.state == RUDP_TRANSFER_FAILED);
    assert(pair.sender.error == RUDP_TRANSFER_ERR_TIMEOUT);

    memset(&pair, 0, sizeof(pair));
    rudp_test_scheduler_init(&pair.scheduler, 1U, 2U);
    assert(rudp_test_scheduler_add_rule(&pair.scheduler, &drop_fin_ack));
    scheduled_pair_start(&pair);
    scheduled_run(&pair, RUDP_FIN_TIMEOUT_MS + 10U);
    assert(pair.sender.state == RUDP_TRANSFER_FAILED);
    assert(pair.sender.error == RUDP_TRANSFER_ERR_COMPLETION_UNKNOWN);
    assert(pair.sink.finished == 1);
}

static void test_invalid_framing(void)
{
    static const uint8_t bytes[1] = {1U};
    struct fake_context context = {0};
    struct memory_sink sink = {0};
    struct rudp_stopwait_receiver receiver;
    const struct rudp_clock clock = {.context = &context, .now_ms = fake_now};
    const struct rudp_session_io io = {.context = &context, .send = fake_send};
    const struct rudp_peer peer = {.ipv4_address = UINT32_C(0x7f000001), .port = 1U};
    const struct rudp_transfer_metadata metadata = {.length = 1025U, .digest = {1U}};
    const struct rudp_transfer_sink sink_api = {
        .context = &sink,
        .append = sink_append,
        .finish = sink_finish,
    };
    const struct rudp_packet short_nonfinal = {
        .type = RUDP_PACKET_DATA,
        .client_nonce = 11U,
        .server_nonce = 22U,
        .data = bytes,
        .data_length = 1U,
    };

    assert(rudp_stopwait_receiver_start(&receiver, &clock, &io, &peer, 11U, 22U, &metadata,
                                        &sink_api) == RUDP_TRANSFER_OK);
    assert(rudp_stopwait_receiver_receive(&receiver, &short_nonfinal) == RUDP_TRANSFER_ERR_PACKET);
    assert(sink.length == 0U);
}

static void test_loss_duplicate_and_completion(void)
{
    uint8_t source[1025];
    struct fake_context sender_context = {0};
    struct fake_context receiver_context = {0};
    struct memory_sink sink = {0};
    struct rudp_stopwait_sender sender;
    struct rudp_stopwait_receiver receiver;
    size_t index;

    for (index = 0U; index < sizeof(source); ++index) {
        source[index] = (uint8_t)index;
    }
    start_pair(&sender, &receiver, &sender_context, &receiver_context, &sink, source,
               sizeof(source));
    assert(sender_context.sent[0].type == RUDP_PACKET_DATA);

    sender_context.now = RUDP_DATA_RETRY_MS;
    assert(rudp_stopwait_sender_tick(&sender) == RUDP_TRANSFER_OK);
    assert(sender_context.sent_count == 2U);
    assert(rudp_stopwait_receiver_receive(&receiver, &sender_context.sent[1]) == RUDP_TRANSFER_OK);
    assert(rudp_stopwait_receiver_receive(&receiver, &sender_context.sent[1]) == RUDP_TRANSFER_OK);
    assert(receiver_context.sent_count == 2U);

    assert(rudp_stopwait_sender_receive(&sender, &receiver_context.sent[1]) == RUDP_TRANSFER_OK);
    assert(sender_context.sent[2].type == RUDP_PACKET_DATA);
    assert(rudp_stopwait_receiver_receive(&receiver, &sender_context.sent[2]) == RUDP_TRANSFER_OK);
    assert(rudp_stopwait_sender_receive(&sender, &receiver_context.sent[2]) == RUDP_TRANSFER_OK);
    assert(sender_context.sent[3].type == RUDP_PACKET_FIN);

    assert(rudp_stopwait_receiver_receive(&receiver, &sender_context.sent[3]) == RUDP_TRANSFER_OK);
    assert(sink.finished == 1);
    sender_context.now += RUDP_DATA_RETRY_MS;
    assert(rudp_stopwait_sender_tick(&sender) == RUDP_TRANSFER_OK);
    assert(sender_context.sent[4].type == RUDP_PACKET_FIN);
    assert(rudp_stopwait_receiver_receive(&receiver, &sender_context.sent[4]) == RUDP_TRANSFER_OK);
    assert(rudp_stopwait_sender_receive(&sender, &receiver_context.sent[4]) == RUDP_TRANSFER_OK);
    assert(sender.state == RUDP_TRANSFER_COMPLETE);
    assert(sink.length == sizeof(source));
    assert(memcmp(sink.bytes, source, sizeof(source)) == 0);

    receiver_context.now = RUDP_LINGER_MS;
    assert(rudp_stopwait_receiver_tick(&receiver) == RUDP_TRANSFER_OK);
    assert(receiver.state == RUDP_TRANSFER_COMPLETE);
}

static void test_empty_and_bounded_failures(void)
{
    static const uint8_t digest[16] = {1U};
    struct fake_context context = {0};
    struct rudp_stopwait_sender sender;
    const struct rudp_clock clock = {.context = &context, .now_ms = fake_now};
    const struct rudp_session_io io = {.context = &context, .send = fake_send};
    const struct rudp_peer peer = {.ipv4_address = UINT32_C(0x7f000001), .port = 1U};

    assert(rudp_stopwait_sender_start(&sender, &clock, &io, &peer, 11U, 22U, NULL, 0U, digest) ==
           RUDP_TRANSFER_OK);
    assert(context.sent[0].type == RUDP_PACKET_FIN);
    context.now = RUDP_FIN_TIMEOUT_MS;
    assert(rudp_stopwait_sender_tick(&sender) == RUDP_TRANSFER_ERR_COMPLETION_UNKNOWN);

    context = (struct fake_context){0};
    assert(rudp_stopwait_sender_start(&sender, &clock, &io, &peer, 11U, 22U, (const uint8_t *)"x",
                                      1U, digest) == RUDP_TRANSFER_OK);
    context.now = RUDP_DATA_PROGRESS_TIMEOUT_MS;
    assert(rudp_stopwait_sender_tick(&sender) == RUDP_TRANSFER_ERR_TIMEOUT);
}

int main(void)
{
    test_loss_duplicate_and_completion();
    test_empty_and_bounded_failures();
    test_scripted_impairments();
    test_directional_randomness();
    test_peer_death_and_completion_unknown();
    test_invalid_framing();
    puts("stop-and-wait tests passed");
    return 0;
}
