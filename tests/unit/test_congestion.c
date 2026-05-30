#include "rudp/congestion.h"

#include <assert.h>
#include <stdio.h>

static void test_ack_accounting_and_small_windows(void)
{
    struct rudp_aimd_cc cc;
    rudp_aimd_init(&cc, 4U);
    assert(rudp_aimd_window(&cc) == 4U);
    rudp_aimd_on_ack(&cc, 20U, 1U);
    assert(cc.cwnd == 4.0);
    rudp_aimd_init(&cc, 100U);
    rudp_aimd_on_ack(&cc, 3U, 3U);
    assert(cc.cwnd == 13.0);
    cc.ssthresh = 13.0;
    rudp_aimd_on_ack(&cc, 13U, 16U);
    assert(cc.cwnd > 13.9 && cc.cwnd < 14.1);
}

static void test_loss_episode_and_timeout_restart(void)
{
    struct rudp_aimd_cc cc;
    rudp_aimd_init(&cc, 100U);
    cc.cwnd = 20.0;
    rudp_aimd_on_fast_loss(&cc, 18U, 30U);
    assert(cc.cwnd == 9.0 && cc.ssthresh == 9.0 && cc.in_recovery);
    rudp_aimd_on_fast_loss(&cc, 4U, 40U);
    assert(cc.cwnd == 9.0 && cc.recovery_boundary == 30U);
    rudp_aimd_on_ack(&cc, 8U, 30U);
    assert(cc.cwnd == 9.0 && cc.in_recovery);
    rudp_aimd_on_ack(&cc, 1U, 31U);
    assert(!cc.in_recovery && cc.cwnd > 9.0);
    rudp_aimd_on_timeout(&cc, 1U, 50U);
    assert(cc.cwnd == 1.0 && cc.ssthresh == 2.0 && cc.in_recovery);
    rudp_aimd_on_timeout(&cc, 40U, 60U);
    assert(cc.cwnd == 1.0 && cc.ssthresh == 20.0 && cc.recovery_boundary == 60U);
}

static void test_idle_and_credit_limited_behavior(void)
{
    struct rudp_aimd_cc cc;
    rudp_aimd_init(&cc, 100U);
    cc.cwnd = 40.0;
    cc.ssthresh = 25.0;
    rudp_aimd_on_data_send(&cc, 100U);
    rudp_aimd_resume_after_idle(&cc, 1100U, 1000.0, true);
    assert(cc.cwnd == 40.0);
    rudp_aimd_resume_after_idle(&cc, 1099U, 1000.0, false);
    assert(cc.cwnd == 40.0);
    rudp_aimd_resume_after_idle(&cc, 1100U, 1000.0, false);
    assert(cc.cwnd == 10.0 && cc.ssthresh == 25.0);
}

static void test_shared_pacer(void)
{
    struct rudp_pacer pacer;
    rudp_pacer_init(&pacer, 0U);
    assert(rudp_pacer_take(&pacer, 0U, 10.0, 1000.0, 1024U));
    assert(rudp_pacer_take(&pacer, 0U, 10.0, 1000.0, 1024U));
    assert(!rudp_pacer_take(&pacer, 0U, 10.0, 1000.0, 1024U));
    assert(rudp_pacer_next_ms(&pacer, 0U, 10.0, 1000.0, 1024U) == 100U);
    assert(rudp_pacer_take(&pacer, 100U, 10.0, 1000.0, 1024U));
}

static void fill_sat_ring(struct rudp_aimd_cc *cc)
{
    uint32_t sequence;

    for (sequence = 0U; sequence < RUDP_SAT_RING_CAPACITY; ++sequence)
        rudp_sat_on_original_send(cc, sequence, sequence, RUDP_SAT_RING_CAPACITY, sequence);
}

static void test_sat_sparse_density_and_duplicate_signals(void)
{
    struct rudp_aimd_cc cc;
    uint32_t sequence;

    rudp_sat_init(&cc, 100U);
    cc.cwnd = 50.0;
    fill_sat_ring(&cc);
    for (sequence = 0U; sequence < 5U; ++sequence)
        rudp_sat_on_fast_loss(&cc, sequence, 50U, 49U);
    assert(cc.sat_marked == 5U && cc.cwnd == 50.0 && !cc.in_recovery);
    rudp_sat_on_fast_loss(&cc, 0U, 50U, 49U);
    assert(cc.sat_marked == 5U && cc.cwnd == 50.0);
    rudp_sat_on_fast_loss(&cc, 5U, 50U, 49U);
    assert(cc.sat_marked == 6U && cc.cwnd == 25.0 && cc.in_recovery);
    rudp_sat_on_fast_loss(&cc, 5U, 50U, 60U);
    assert(cc.sat_marked == 6U && cc.recovery_boundary == 49U);
}

static void test_sat_eviction_recovery_and_timeout(void)
{
    struct rudp_aimd_cc cc;
    uint32_t sequence;

    rudp_sat_init(&cc, 100U);
    cc.cwnd = 50.0;
    fill_sat_ring(&cc);
    rudp_sat_on_original_send(&cc, 50U, 50U, 50U, 50U);
    rudp_sat_on_fast_loss(&cc, 0U, 50U, 50U);
    assert(cc.sat_marked == 0U && cc.cwnd == 50.0);
    for (sequence = 1U; sequence <= 6U; ++sequence)
        rudp_sat_on_fast_loss(&cc, sequence, 50U, 50U);
    assert(cc.in_recovery && cc.cwnd == 25.0);
    rudp_aimd_on_ack(&cc, 1U, 51U);
    assert(!cc.in_recovery);
    rudp_sat_on_fast_loss(&cc, 7U, 50U, 51U);
    assert(cc.in_recovery && cc.cwnd == 25.0);
    rudp_aimd_on_timeout(&cc, 50U, 52U);
    assert(cc.cwnd == 1.0 && cc.ssthresh == 25.0 && cc.in_recovery);
}

int main(void)
{
    test_ack_accounting_and_small_windows();
    test_loss_episode_and_timeout_restart();
    test_idle_and_credit_limited_behavior();
    test_shared_pacer();
    test_sat_sparse_density_and_duplicate_signals();
    test_sat_eviction_recovery_and_timeout();
    puts("AIMD/Sat congestion-control and pacing tests passed");
    return 0;
}
