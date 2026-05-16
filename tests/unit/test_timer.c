#include "rudp/timer.h"

#include <assert.h>
#include <stdio.h>

static void assert_close(double actual, double expected)
{
    const double difference = actual > expected ? actual - expected : expected - actual;

    assert(difference < 0.000001);
}

static void test_independent_estimator_trace(void)
{
    struct rudp_rtt_estimator estimator;

    rudp_rtt_estimator_init(&estimator);
    assert(!estimator.initialized);
    assert_close(estimator.current_rto_ms, 1000.0);

    rudp_rtt_estimator_sample(&estimator, 100.0);
    assert(estimator.initialized);
    assert_close(estimator.srtt_ms, 100.0);
    assert_close(estimator.rttvar_ms, 50.0);
    assert_close(estimator.computed_rto_ms, 300.0);
    assert_close(estimator.current_rto_ms, 300.0);

    /* Independently: variance=.75*50+.25*20=42.5; SRTT=.875*100+.125*120=102.5. */
    rudp_rtt_estimator_sample(&estimator, 120.0);
    assert_close(estimator.rttvar_ms, 42.5);
    assert_close(estimator.srtt_ms, 102.5);
    assert_close(estimator.computed_rto_ms, 272.5);

    /* The old SRTT (102.5), not the updated SRTT, gives variance 32.5. */
    rudp_rtt_estimator_sample(&estimator, 100.0);
    assert_close(estimator.rttvar_ms, 32.5);
    assert_close(estimator.srtt_ms, 102.1875);
    assert_close(estimator.computed_rto_ms, 232.1875);
}

static void test_bounds_backoff_and_clean_restore(void)
{
    struct rudp_rtt_estimator estimator;
    unsigned int index;

    rudp_rtt_estimator_init(&estimator);
    rudp_rtt_estimator_sample(&estimator, 0.0);
    assert_close(estimator.computed_rto_ms, 100.0);
    for (index = 0U; index < 16U; ++index) {
        rudp_rtt_estimator_timeout(&estimator);
    }
    assert_close(estimator.current_rto_ms, 60000.0);
    rudp_rtt_estimator_sample(&estimator, 0.0);
    assert_close(estimator.current_rto_ms, estimator.computed_rto_ms);
    assert_close(estimator.current_rto_ms, 100.0);
}

int main(void)
{
    test_independent_estimator_trace();
    test_bounds_backoff_and_clean_restore();
    puts("adaptive timer estimator tests passed");
    return 0;
}
