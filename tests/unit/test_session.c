#include "rudp/session.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fake_context {
    uint64_t now;
    uint64_t nonce;
    int random_failure;
    int send_failure;
    struct rudp_packet sent[32];
    size_t sent_count;
};

static uint64_t fake_now(void *context)
{
    return ((struct fake_context *)context)->now;
}

static int fake_random(void *context, uint8_t *output, size_t output_length)
{
    struct fake_context *fake = context;

    if (fake->random_failure != 0) {
        return -1;
    }
    assert(output_length == sizeof(fake->nonce));
    memcpy(output, &fake->nonce, output_length);
    fake->nonce += 1U;
    return 0;
}

static int fake_send(void *context, const struct rudp_peer *peer, const struct rudp_packet *packet)
{
    struct fake_context *fake = context;

    assert(peer->ipv4_address == UINT32_C(0x7f000001));
    assert(peer->port != 0U);
    if (fake->send_failure != 0) {
        return -1;
    }
    assert(fake->sent_count < sizeof(fake->sent) / sizeof(fake->sent[0]));
    fake->sent[fake->sent_count++] = *packet;
    return 0;
}

static struct rudp_clock clock_for(struct fake_context *context)
{
    return (struct rudp_clock){
        .context = context,
        .now_ms = fake_now,
    };
}

static struct rudp_random random_for(struct fake_context *context)
{
    return (struct rudp_random){
        .context = context,
        .bytes = fake_random,
    };
}

static struct rudp_session_io io_for(struct fake_context *context)
{
    return (struct rudp_session_io){
        .context = context,
        .send = fake_send,
    };
}

static struct rudp_peer peer(uint16_t port)
{
    return (struct rudp_peer){
        .ipv4_address = UINT32_C(0x7f000001),
        .port = port,
    };
}

static struct rudp_transfer_metadata metadata(void)
{
    struct rudp_transfer_metadata result;
    size_t index;

    memset(&result, 0, sizeof(result));
    result.length = 42U;
    for (index = 0U; index < sizeof(result.digest); ++index) {
        result.digest[index] = (uint8_t)index;
    }
    return result;
}

static void establish(struct rudp_session *sender, struct rudp_session *receiver,
                      struct fake_context *sender_context, struct fake_context *receiver_context,
                      const struct rudp_peer *sender_peer, const struct rudp_peer *receiver_peer)
{
    const struct rudp_clock sender_clock = clock_for(sender_context);
    const struct rudp_clock receiver_clock = clock_for(receiver_context);
    const struct rudp_random sender_random = random_for(sender_context);
    const struct rudp_random receiver_random = random_for(receiver_context);
    const struct rudp_session_io sender_io = io_for(sender_context);
    const struct rudp_session_io receiver_io = io_for(receiver_context);
    const struct rudp_transfer_metadata transfer = metadata();

    assert(rudp_receiver_listen(receiver, &receiver_clock, &receiver_random, &receiver_io) ==
           RUDP_SESSION_OK);
    assert(rudp_sender_start(sender, &sender_clock, &sender_random, &sender_io, receiver_peer,
                             &transfer) == RUDP_SESSION_OK);
    assert(sender_context->sent_count == 1U);
    assert(sender_context->sent[0].type == RUDP_PACKET_SYN);
    assert(rudp_session_receive(receiver, sender_peer, &sender_context->sent[0]) ==
           RUDP_SESSION_OK);
    assert(receiver_context->sent_count == 1U);
    assert(receiver_context->sent[0].type == RUDP_PACKET_SYN_ACK);
    assert(rudp_session_receive(sender, receiver_peer, &receiver_context->sent[0]) ==
           RUDP_SESSION_OK);
    assert(sender_context->sent_count == 2U);
    assert(sender_context->sent[1].type == RUDP_PACKET_OPEN);
    assert(rudp_session_receive(receiver, sender_peer, &sender_context->sent[1]) ==
           RUDP_SESSION_OK);
    assert(receiver_context->sent_count == 2U);
    assert(receiver_context->sent[1].type == RUDP_PACKET_OPEN_ACK);
    assert(rudp_session_receive(sender, receiver_peer, &receiver_context->sent[1]) ==
           RUDP_SESSION_OK);
    assert(sender->state == RUDP_SESSION_ESTABLISHED);
    assert(receiver->state == RUDP_SESSION_ESTABLISHED);
}

static void test_setup_duplicates_and_peer_binding(void)
{
    struct fake_context sender_context = {.nonce = 10U};
    struct fake_context receiver_context = {.nonce = 20U};
    struct rudp_session sender;
    struct rudp_session receiver;
    const struct rudp_peer sender_peer = peer(40001U);
    const struct rudp_peer receiver_peer = peer(40002U);
    uint64_t pending_deadline;

    establish(&sender, &receiver, &sender_context, &receiver_context, &sender_peer, &receiver_peer);

    assert(rudp_session_receive(&receiver, &sender_peer, &sender_context.sent[0]) ==
           RUDP_SESSION_OK);
    assert(receiver_context.sent_count == 3U);
    assert(receiver_context.sent[2].type == RUDP_PACKET_SYN_ACK);
    assert(rudp_session_receive(&sender, &receiver_peer, &receiver_context.sent[0]) ==
           RUDP_SESSION_OK);
    assert(sender_context.sent_count == 3U);
    assert(sender_context.sent[2].type == RUDP_PACKET_OPEN);

    pending_deadline = receiver.setup_deadline_ms;
    assert(rudp_session_receive(&receiver,
                                &(struct rudp_peer){
                                    .ipv4_address = UINT32_C(0x7f000001),
                                    .port = 49999U,
                                },
                                &sender_context.sent[1]) == RUDP_SESSION_ERR_PEER);
    assert(receiver.setup_deadline_ms == pending_deadline);
}

static void test_retries_timeout_and_failures(void)
{
    struct fake_context context = {.nonce = 5U};
    struct rudp_session sender;
    const struct rudp_clock clock = clock_for(&context);
    const struct rudp_random random = random_for(&context);
    const struct rudp_session_io io = io_for(&context);
    const struct rudp_peer destination = peer(40002U);
    const struct rudp_transfer_metadata transfer = metadata();

    assert(rudp_sender_start(&sender, &clock, &random, &io, &destination, &transfer) ==
           RUDP_SESSION_OK);
    assert(sender.next_retry_ms == 1000U);
    context.now = 1000U;
    assert(rudp_session_tick(&sender) == RUDP_SESSION_OK);
    assert(context.sent_count == 2U);
    assert(sender.next_retry_ms == 3000U);

    {
        struct fake_context timeout_context = {.nonce = 5U};
        const struct rudp_clock timeout_clock = clock_for(&timeout_context);
        const struct rudp_random timeout_random = random_for(&timeout_context);
        const struct rudp_session_io timeout_io = io_for(&timeout_context);

        assert(rudp_sender_start(&sender, &timeout_clock, &timeout_random, &timeout_io,
                                 &destination, &transfer) == RUDP_SESSION_OK);
        timeout_context.now = 30000U;
        assert(rudp_session_tick(&sender) == RUDP_SESSION_ERR_TIMEOUT);
        assert(sender.state == RUDP_SESSION_FAILED);
    }

    {
        struct fake_context random_context = {.nonce = 5U, .random_failure = 1};
        const struct rudp_clock random_clock = clock_for(&random_context);
        const struct rudp_random failing_random = random_for(&random_context);
        const struct rudp_session_io random_io = io_for(&random_context);

        assert(rudp_sender_start(&sender, &random_clock, &failing_random, &random_io, &destination,
                                 &transfer) == RUDP_SESSION_ERR_RANDOM);
    }
    {
        struct fake_context send_context = {.nonce = 5U, .send_failure = 1};
        const struct rudp_clock send_clock = clock_for(&send_context);
        const struct rudp_random send_random = random_for(&send_context);
        const struct rudp_session_io failing_io = io_for(&send_context);

        assert(rudp_sender_start(&sender, &send_clock, &send_random, &failing_io, &destination,
                                 &transfer) == RUDP_SESSION_ERR_SEND);
    }
}

static void test_restart_abort_and_limits(void)
{
    struct fake_context sender_context = {.nonce = 10U};
    struct fake_context receiver_context = {.nonce = 20U};
    struct rudp_session sender;
    struct rudp_session receiver;
    struct rudp_session restarted_receiver;
    const struct rudp_peer sender_peer = peer(40001U);
    const struct rudp_peer receiver_peer = peer(40002U);
    const struct rudp_clock receiver_clock = clock_for(&receiver_context);
    const struct rudp_random receiver_random = random_for(&receiver_context);
    const struct rudp_session_io receiver_io = io_for(&receiver_context);

    establish(&sender, &receiver, &sender_context, &receiver_context, &sender_peer, &receiver_peer);
    assert(rudp_session_abort(&sender, 17U) == RUDP_SESSION_ERR_ABORTED);
    assert(sender_context.sent[sender_context.sent_count - 1U].type == RUDP_PACKET_ABORT);
    assert(rudp_session_receive(&receiver, &sender_peer,
                                &sender_context.sent[sender_context.sent_count - 1U]) ==
           RUDP_SESSION_ERR_ABORTED);
    assert(receiver.state == RUDP_SESSION_FAILED);

    assert(rudp_receiver_listen(&restarted_receiver, &receiver_clock, &receiver_random,
                                &receiver_io) == RUDP_SESSION_OK);
    assert(rudp_session_receive(&restarted_receiver, &sender_peer, &sender_context.sent[1]) ==
           RUDP_SESSION_ERR_PACKET);
}

int main(void)
{
    test_setup_duplicates_and_peer_binding();
    test_retries_timeout_and_failures();
    test_restart_abort_and_limits();
    puts("session tests passed");
    return 0;
}
