#include "rudp/timer.h"

static double clamp_rto(double value)
{
    if (value < RUDP_MIN_RTO_MS) {
        return RUDP_MIN_RTO_MS;
    }
    if (value > RUDP_MAX_RTO_MS) {
        return RUDP_MAX_RTO_MS;
    }
    return value;
}

void rudp_rtt_estimator_init(struct rudp_rtt_estimator *estimator)
{
    estimator->srtt_ms = 0.0;
    estimator->rttvar_ms = 0.0;
    estimator->computed_rto_ms = RUDP_INITIAL_RTO_MS;
    estimator->current_rto_ms = RUDP_INITIAL_RTO_MS;
    estimator->initialized = false;
}

void rudp_rtt_estimator_sample(struct rudp_rtt_estimator *estimator, double sample_ms)
{
    double margin;

    if (!estimator->initialized) {
        estimator->srtt_ms = sample_ms;
        estimator->rttvar_ms = sample_ms / 2.0;
        estimator->initialized = true;
    } else {
        double variation = estimator->srtt_ms - sample_ms;

        if (variation < 0.0) {
            variation = -variation;
        }
        estimator->rttvar_ms = 0.75 * estimator->rttvar_ms + 0.25 * variation;
        estimator->srtt_ms = 0.875 * estimator->srtt_ms + 0.125 * sample_ms;
    }
    margin = 4.0 * estimator->rttvar_ms;
    if (margin < 1.0) {
        margin = 1.0;
    }
    estimator->computed_rto_ms = clamp_rto(estimator->srtt_ms + margin);
    estimator->current_rto_ms = estimator->computed_rto_ms;
}

void rudp_rtt_estimator_timeout(struct rudp_rtt_estimator *estimator)
{
    estimator->current_rto_ms = clamp_rto(estimator->current_rto_ms * 2.0);
}
