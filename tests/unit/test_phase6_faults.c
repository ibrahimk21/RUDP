#include "rudp/file.h"
#include "rudp/io.h"
#include "rudp/packet.h"
#include "rudp/session.h"
#include "rudp/window.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct short_writer {
    uint8_t bytes[32];
    size_t length;
    bool interrupt_once;
    bool return_zero;
};

struct session_fixture {
    uint64_t now_ms;
    uint64_t nonce;
    unsigned int sends;
};

static ssize_t short_write(void *context, const uint8_t *bytes, size_t length)
{
    struct short_writer *writer = context;
    size_t count;

    if (writer->interrupt_once) {
        writer->interrupt_once = false;
        errno = EINTR;
        return -1;
    }
    if (writer->return_zero) {
        return 0;
    }
    count = length > 3U ? 3U : length;
    memcpy(writer->bytes + writer->length, bytes, count);
    writer->length += count;
    return (ssize_t)count;
}

static uint64_t fixture_now(void *context)
{
    return ((struct session_fixture *)context)->now_ms;
}

static int fixture_random(void *context, uint8_t *output, size_t length)
{
    struct session_fixture *fixture = context;

    assert(length == sizeof(fixture->nonce));
    memcpy(output, &fixture->nonce, length);
    fixture->nonce += 1U;
    return 0;
}

static int fixture_send(void *context, const struct rudp_peer *peer,
                        const struct rudp_packet *packet)
{
    struct session_fixture *fixture = context;

    (void)peer;
    (void)packet;
    fixture->sends += 1U;
    return 0;
}

static void test_short_io_and_disk_full(void)
{
    static const uint8_t bytes[] = "short writes are retried";
    struct short_writer writer = {.interrupt_once = true};
    struct rudp_output_file output;
    int result;

    result = rudp_write_all(short_write, &writer, bytes, sizeof(bytes));
    assert(result == 0);
    assert(writer.length == sizeof(bytes));
    assert(memcmp(writer.bytes, bytes, sizeof(bytes)) == 0);
    writer.return_zero = true;
    errno = 0;
    result = rudp_write_all(short_write, &writer, bytes, 1U);
    assert(result != 0 && errno == EIO);

    memset(&output, 0, sizeof(output));
    output.fd = open("/dev/full", O_WRONLY);
    assert(output.fd >= 0);
    rudp_md5_init(&output.md5);
    assert(rudp_output_append(&output, bytes, sizeof(bytes)) != 0);
    assert(output.length == 0U);
    rudp_output_abort(&output);
}

static void test_corruption_control_loss_and_restart(void)
{
    struct session_fixture fixture = {.nonce = 100U};
    const struct rudp_clock clock = {&fixture, fixture_now};
    const struct rudp_random random = {&fixture, fixture_random};
    const struct rudp_session_io io = {&fixture, fixture_send};
    const struct rudp_peer peer = {UINT32_C(0x7f000001), 9000U};
    struct rudp_session first;
    struct rudp_session restarted;
    struct rudp_transfer_metadata metadata = {.length = 1U};
    struct rudp_packet syn = {
        .type = RUDP_PACKET_SYN,
        .client_nonce = 55U,
        .transfer_length = 1U,
    };
    struct rudp_packet old_open;
    uint8_t encoded[RUDP_MAX_DATAGRAM_SIZE];
    size_t encoded_length;

    assert(rudp_packet_encode(&syn, encoded, sizeof(encoded), &encoded_length) == RUDP_PACKET_OK);
    encoded[encoded_length - 1U] ^= 1U;
    assert(rudp_packet_decode(&old_open, encoded, encoded_length) == RUDP_PACKET_ERR_CHECKSUM);

    assert(rudp_receiver_listen(&first, &clock, &random, &io) == RUDP_SESSION_OK);
    assert(rudp_session_receive(&first, &peer, &syn) == RUDP_SESSION_OK);
    assert(fixture.sends == 1U);
    fixture.now_ms = 1000U;
    assert(rudp_session_tick(&first) == RUDP_SESSION_OK && fixture.sends == 2U);
    old_open = (struct rudp_packet){
        .type = RUDP_PACKET_OPEN,
        .client_nonce = first.client_nonce,
        .server_nonce = first.server_nonce,
    };
    assert(rudp_receiver_listen(&restarted, &clock, &random, &io) == RUDP_SESSION_OK);
    assert(rudp_session_receive(&restarted, &peer, &syn) == RUDP_SESSION_OK);
    assert(restarted.server_nonce != first.server_nonce);
    assert(rudp_session_receive(&restarted, &peer, &old_open) == RUDP_SESSION_ERR_PACKET);

    metadata.length = RUDP_MAX_TRANSFER_LENGTH + 1U;
    assert(rudp_sender_start(&first, &clock, &random, &io, &peer, &metadata) ==
           RUDP_SESSION_ERR_TRANSFER_SIZE);
}

static void test_synthetic_wrap(void)
{
    struct rudp_receive_window *window = calloc(1U, sizeof(*window));
    uint8_t byte = 1U;

    assert(window != NULL);
    rudp_receive_window_init(window, UINT32_MAX);
    assert(rudp_receive_window_insert(window, UINT32_MAX, &byte, 1U));
    assert(rudp_receive_window_insert(window, 0U, &byte, 1U));
    assert(window->expected == 1U);
    free(window);
}

int main(void)
{
    test_short_io_and_disk_full();
    test_corruption_control_loss_and_restart();
    test_synthetic_wrap();
    puts("phase 6 targeted cases passed: 7 (short-I/O, disk-full, corruption, control-loss, "
         "restart, size-limit, synthetic-wrap)");
    return 0;
}
