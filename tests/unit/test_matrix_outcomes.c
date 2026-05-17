#include "rudp/md5.h"
#include "rudp/transfer.h"
#include "scheduler.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct matrix_case {
    struct rudp_test_scheduler *scheduler;
    struct rudp_windowed_sender *sender;
    struct rudp_windowed_receiver *receiver;
    struct rudp_test_link forward;
    struct rudp_test_link reverse;
    uint8_t *source;
    uint8_t *output;
    size_t length;
    size_t output_length;
    bool finished;
};

static int append_output(void *context, const uint8_t *bytes, size_t length)
{
    struct matrix_case *test = context;

    if (test->output_length + length > test->length) {
        return -1;
    }
    memcpy(test->output + test->output_length, bytes, length);
    test->output_length += length;
    return 0;
}

static int finish_output(void *context, uint64_t length, const uint8_t digest[16])
{
    struct matrix_case *test = context;
    uint8_t actual[16];

    rudp_md5_bytes(test->output, test->output_length, actual);
    if (length != test->output_length || memcmp(actual, digest, sizeof(actual)) != 0) {
        return -1;
    }
    test->finished = true;
    return 0;
}

static void deliver(void *context, enum rudp_test_direction direction,
                    const struct rudp_packet *packet, bool corrupted)
{
    struct matrix_case *test = context;

    assert(!corrupted);
    if (direction == RUDP_TEST_FORWARD) {
        size_t consumed;

        (void)rudp_windowed_receiver_receive(test->receiver, packet);
        if (packet->type == RUDP_PACKET_DATA) {
            (void)rudp_windowed_receiver_consume(test->receiver, RUDP_WINDOW_CAPACITY, &consumed);
        }
    } else {
        (void)rudp_windowed_sender_receive(test->sender, packet);
    }
}

static void print_trace(const struct matrix_case *test)
{
    size_t index;

    for (index = 0U; index < test->scheduler->trace_count; ++index) {
        const struct rudp_test_trace_event *event = &test->scheduler->trace[index];

        fprintf(stderr,
                "trace direction=%u number=%llu at=%llu due=%llu type=%u seq=%u drop=%u "
                "duplicate=%u reorder=%u\n",
                (unsigned int)event->direction, (unsigned long long)event->number,
                (unsigned long long)event->at_ms, (unsigned long long)event->due_ms,
                (unsigned int)event->type, event->sequence, event->dropped ? 1U : 0U,
                event->duplicated ? 1U : 0U, event->reordered ? 1U : 0U);
    }
    if (test->scheduler->trace_truncated) {
        fputs("trace truncated\n", stderr);
    }
}

enum case_outcome {
    CASE_SUCCESS,
    CASE_TIMEOUT,
    CASE_COMPLETION_UNKNOWN,
};

static enum case_outcome run_case(enum rudp_test_profile profile, enum rudp_test_mode mode,
                                  int loss, size_t length, int expected)
{
    struct matrix_case test;
    const struct rudp_peer peer = {UINT32_C(0x7f000001), 1U};
    struct rudp_clock clock;
    struct rudp_session_io sender_io;
    struct rudp_session_io receiver_io;
    struct rudp_transfer_metadata metadata = {.length = length};
    struct rudp_transfer_sink sink;
    uint64_t now;
    size_t index;

    memset(&test, 0, sizeof(test));
    test.scheduler = calloc(1U, sizeof(*test.scheduler));
    test.sender = calloc(1U, sizeof(*test.sender));
    test.receiver = calloc(1U, sizeof(*test.receiver));
    test.source = malloc(length == 0U ? 1U : length);
    test.output = malloc(length == 0U ? 1U : length);
    assert(test.scheduler != NULL && test.sender != NULL && test.receiver != NULL &&
           test.source != NULL && test.output != NULL);
    test.length = length;
    for (index = 0U; index < length; ++index) {
        test.source[index] = (uint8_t)(index * 29U + 7U);
    }
    rudp_md5_bytes(test.source, length, metadata.digest);
    rudp_test_scheduler_init(test.scheduler, 100U, 200U);
    rudp_test_scheduler_set_impairment(test.scheduler,
                                       &(struct rudp_test_impairment){profile, mode, loss});
    test.forward = (struct rudp_test_link){test.scheduler, RUDP_TEST_FORWARD};
    test.reverse = (struct rudp_test_link){test.scheduler, RUDP_TEST_REVERSE};
    clock = (struct rudp_clock){test.scheduler, rudp_test_scheduler_now};
    sender_io = (struct rudp_session_io){&test.forward, rudp_test_scheduler_send};
    receiver_io = (struct rudp_session_io){&test.reverse, rudp_test_scheduler_send};
    sink = (struct rudp_transfer_sink){&test, append_output, finish_output};
    assert(rudp_windowed_receiver_start(test.receiver, &clock, &receiver_io, &peer, 11U, 22U,
                                        &metadata, &sink) == RUDP_TRANSFER_OK);
    assert(rudp_windowed_sender_start(test.sender, &clock, &sender_io, &peer, 11U, 22U, test.source,
                                      length, metadata.digest, 32U,
                                      RUDP_WINDOW_CAPACITY) == RUDP_TRANSFER_OK);
    for (now = 0U; now <= RUDP_TRANSFER_TIMEOUT_MS + 1000U; now += 10U) {
        rudp_test_scheduler_advance(test.scheduler, now, deliver, &test);
        (void)rudp_windowed_sender_tick(test.sender);
        (void)rudp_windowed_receiver_tick(test.receiver);
        rudp_test_scheduler_advance(test.scheduler, now, deliver, &test);
        if (test.sender->state == RUDP_TRANSFER_COMPLETE ||
            test.sender->state == RUDP_TRANSFER_FAILED) {
            break;
        }
    }
    {
        bool success = test.sender->state == RUDP_TRANSFER_COMPLETE && test.finished &&
                       test.output_length == length &&
                       memcmp(test.output, test.source, length) == 0;
        enum case_outcome outcome = CASE_SUCCESS;

        if (!success) {
            assert(test.sender->state == RUDP_TRANSFER_FAILED);
            outcome = test.sender->error == RUDP_TRANSFER_ERR_COMPLETION_UNKNOWN
                          ? CASE_COMPLETION_UNKNOWN
                          : CASE_TIMEOUT;
            assert(test.sender->error == RUDP_TRANSFER_ERR_TIMEOUT ||
                   test.sender->error == RUDP_TRANSFER_ERR_COMPLETION_UNKNOWN);
            if (outcome == CASE_TIMEOUT) {
                assert(!test.finished);
            } else if (test.finished) {
                assert(test.finished && test.output_length == length &&
                       memcmp(test.output, test.source, length) == 0);
            }
        }
        if (expected >= 0 && outcome != (enum case_outcome)expected) {
            fprintf(stderr,
                    "matrix mismatch profile=%s mode=%s loss=%d length=%zu expected=%d actual=%d\n",
                    rudp_test_profile_name(profile), rudp_test_mode_name(mode), loss, length,
                    expected, (int)outcome);
            print_trace(&test);
        }
        if (loss == 100) {
            assert(outcome != CASE_SUCCESS && test.output_length == 0U);
        }
        free(test.output);
        free(test.source);
        free(test.receiver);
        free(test.sender);
        free(test.scheduler);
        assert(expected < 0 || outcome == (enum case_outcome)expected);
        return outcome;
    }
}

int main(void)
{
    static const enum rudp_test_profile profiles[] = {RUDP_TEST_PROFILE_TERRESTRIAL,
                                                      RUDP_TEST_PROFILE_GEO, RUDP_TEST_PROFILE_LEO};
    static const int losses[] = {0, 1, 5, 10, 25, 50, 75, 100};
    static const enum rudp_test_mode modes[] = {
        RUDP_TEST_MODE_NONE,
        RUDP_TEST_MODE_REORDER,
        RUDP_TEST_MODE_DUPLICATE,
        RUDP_TEST_MODE_COMBINED,
    };
    static const size_t lengths[] = {0U, 1U, 1023U, 1024U, 1025U, 64U * 1024U, 1024U * 1024U};
    size_t profile_index;
    size_t loss_index;
    size_t mode_index;
    size_t length_index;
    size_t random_cases = 0U;
    size_t native_cases = 0U;

    for (profile_index = 0U; profile_index < 3U; ++profile_index) {
        for (loss_index = 0U; loss_index < 8U; ++loss_index) {
            for (mode_index = 0U; mode_index < 4U; ++mode_index) {
                for (length_index = 0U; length_index < 7U; ++length_index) {
                    enum case_outcome expected =
                        run_case(profiles[profile_index], modes[mode_index], losses[loss_index],
                                 lengths[length_index], -1);

                    assert(run_case(profiles[profile_index], modes[mode_index], losses[loss_index],
                                    lengths[length_index], (int)expected) == expected);
                    random_cases += 1U;
                }
            }
        }
        for (mode_index = 0U; mode_index < 4U; ++mode_index) {
            for (length_index = 0U; length_index < 7U; ++length_index) {
                enum case_outcome expected = run_case(profiles[profile_index], modes[mode_index],
                                                      -1, lengths[length_index], -1);

                assert(run_case(profiles[profile_index], modes[mode_index], -1,
                                lengths[length_index], (int)expected) == expected);
                native_cases += 1U;
            }
        }
    }
    printf("matrix outcomes passed: %zu random overrides, %zu native profile cases\n", random_cases,
           native_cases);
    return 0;
}
