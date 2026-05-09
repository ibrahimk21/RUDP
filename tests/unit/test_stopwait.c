#include "rudp/stopwait.h"

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
    puts("stop-and-wait tests passed");
    return 0;
}
