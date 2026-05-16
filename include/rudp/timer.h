#ifndef RUDP_TIMER_H
#define RUDP_TIMER_H

#include <stdbool.h>

#define RUDP_INITIAL_RTO_MS 1000.0
#define RUDP_MIN_RTO_MS 100.0
#define RUDP_MAX_RTO_MS 60000.0

struct rudp_rtt_estimator {
    double srtt_ms;
    double rttvar_ms;
    double computed_rto_ms;
    double current_rto_ms;
    bool initialized;
};

void rudp_rtt_estimator_init(struct rudp_rtt_estimator *estimator);
void rudp_rtt_estimator_sample(struct rudp_rtt_estimator *estimator, double sample_ms);
void rudp_rtt_estimator_timeout(struct rudp_rtt_estimator *estimator);

#endif
