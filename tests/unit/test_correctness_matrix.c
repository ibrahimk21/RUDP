#include "scheduler.h"

#include <assert.h>
#include <stdio.h>

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
    size_t cases = 0U;
    struct rudp_test_scheduler scheduler;
    struct rudp_test_link forward = {&scheduler, RUDP_TEST_FORWARD};
    struct rudp_test_link reverse = {&scheduler, RUDP_TEST_REVERSE};
    const struct rudp_peer peer = {0U, 1U};
    const struct rudp_packet packet = {
        .type = RUDP_PACKET_PROBE,
        .client_nonce = 1U,
        .server_nonce = 2U,
    };

    assert(rudp_test_profile_one_way_ms(RUDP_TEST_PROFILE_TERRESTRIAL) == 25U);
    assert(rudp_test_profile_one_way_ms(RUDP_TEST_PROFILE_GEO) == 300U);
    assert(rudp_test_profile_one_way_ms(RUDP_TEST_PROFILE_LEO) == 20U);
    for (profile_index = 0U; profile_index < sizeof(profiles) / sizeof(profiles[0]);
         ++profile_index) {
        for (loss_index = 0U; loss_index < sizeof(losses) / sizeof(losses[0]); ++loss_index) {
            for (mode_index = 0U; mode_index < sizeof(modes) / sizeof(modes[0]); ++mode_index) {
                for (length_index = 0U; length_index < sizeof(lengths) / sizeof(lengths[0]);
                     ++length_index) {
                    const struct rudp_test_impairment impairment = {
                        profiles[profile_index],
                        modes[mode_index],
                        losses[loss_index],
                    };

                    rudp_test_scheduler_init(&scheduler, UINT32_C(0x13579bdf),
                                             UINT32_C(0x2468ace1));
                    rudp_test_scheduler_set_impairment(&scheduler, &impairment);
                    assert(scheduler.impairment.profile == profiles[profile_index]);
                    assert(scheduler.impairment.mode == modes[mode_index]);
                    assert(scheduler.impairment.random_loss_percent == losses[loss_index]);
                    (void)lengths[length_index];
                    cases += 1U;
                }
            }
        }
    }
    assert(cases == 672U);

    rudp_test_scheduler_init(&scheduler, 1U, 2U);
    rudp_test_scheduler_set_impairment(
        &scheduler,
        &(struct rudp_test_impairment){RUDP_TEST_PROFILE_GEO, RUDP_TEST_MODE_COMBINED, 0});
    for (cases = 0U; cases < 11U; ++cases) {
        assert(rudp_test_scheduler_send(&forward, &peer, &packet) == 0);
        assert(rudp_test_scheduler_send(&reverse, &peer, &packet) == 0);
    }
    assert(scheduler.original_datagrams[RUDP_TEST_FORWARD] == 11U);
    assert(scheduler.original_datagrams[RUDP_TEST_REVERSE] == 11U);
    assert(scheduler.trace[12U].number == 7U && scheduler.trace[12U].reordered &&
           scheduler.trace[12U].due_ms == 1500U);
    assert(scheduler.trace[20U].number == 11U && scheduler.trace[20U].duplicated);
    assert(scheduler.queued_count == 24U);

    rudp_test_scheduler_init(&scheduler, 1U, 2U);
    rudp_test_scheduler_set_impairment(
        &scheduler,
        &(struct rudp_test_impairment){RUDP_TEST_PROFILE_LEO, RUDP_TEST_MODE_NONE, 100});
    assert(rudp_test_scheduler_send(&forward, &peer, &packet) == 0);
    assert(scheduler.trace[0].dropped && scheduler.queued_count == 0U);
    printf("correctness matrix defined: %u random-loss cases plus native burst cases\n", 672U);
    return 0;
}
