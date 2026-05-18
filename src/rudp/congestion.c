#include "rudp/congestion.h"

#include "rudp/packet.h"
#include "rudp/window.h"

static double clamp_window(double value, uint32_t maximum)
{
    if (value < 1.0)
        return 1.0;
    if (value > (double)maximum)
        return (double)maximum;
    return value;
}

void rudp_fixed_cc_init(struct rudp_fixed_cc *cc, uint32_t window)
{
    cc->window = window == 0U ? 1U : window;
    if (cc->window > RUDP_WINDOW_CAPACITY)
        cc->window = RUDP_WINDOW_CAPACITY;
}

uint32_t rudp_fixed_cc_window(const struct rudp_fixed_cc *cc)
{
    return cc->window;
}

void rudp_aimd_init(struct rudp_aimd_cc *cc, uint32_t maximum_window)
{
    if (maximum_window == 0U || maximum_window > RUDP_WINDOW_CAPACITY)
        maximum_window = RUDP_WINDOW_CAPACITY;
    *cc = (struct rudp_aimd_cc){
        .cwnd = maximum_window < RUDP_AIMD_INITIAL_WINDOW ? (double)maximum_window
                                                          : RUDP_AIMD_INITIAL_WINDOW,
        .ssthresh = (double)maximum_window,
        .maximum_window = maximum_window,
    };
}

uint32_t rudp_aimd_window(const struct rudp_aimd_cc *cc)
{
    uint32_t window = (uint32_t)cc->cwnd;
    return window == 0U ? 1U : window;
}

static void leave_recovery(struct rudp_aimd_cc *cc, uint32_t cumulative_ack)
{
    if (cc->in_recovery && rudp_seq_after(cumulative_ack, cc->recovery_boundary))
        cc->in_recovery = false;
}

void rudp_aimd_on_ack(struct rudp_aimd_cc *cc, uint32_t newly_acked, uint32_t cumulative_ack)
{
    uint32_t index;
    leave_recovery(cc, cumulative_ack);
    if (cc->in_recovery)
        return;
    for (index = 0U; index < newly_acked; ++index) {
        cc->cwnd += cc->cwnd < cc->ssthresh ? 1.0 : 1.0 / cc->cwnd;
        cc->cwnd = clamp_window(cc->cwnd, cc->maximum_window);
    }
}

static double reduced_threshold(size_t flight, uint32_t maximum)
{
    double threshold = (double)flight / 2.0;
    if (threshold < 2.0)
        threshold = 2.0;
    return clamp_window(threshold, maximum);
}

void rudp_aimd_on_fast_loss(struct rudp_aimd_cc *cc, size_t flight, uint32_t boundary)
{
    if (cc->in_recovery)
        return;
    cc->ssthresh = reduced_threshold(flight, cc->maximum_window);
    cc->cwnd = cc->ssthresh;
    cc->recovery_boundary = boundary;
    cc->in_recovery = true;
}

void rudp_aimd_on_timeout(struct rudp_aimd_cc *cc, size_t flight, uint32_t boundary)
{
    cc->ssthresh = reduced_threshold(flight, cc->maximum_window);
    cc->cwnd = 1.0;
    cc->recovery_boundary = boundary;
    cc->in_recovery = true;
}

void rudp_aimd_on_data_send(struct rudp_aimd_cc *cc, uint64_t now_ms)
{
    cc->last_data_send_ms = now_ms;
    cc->has_sent = true;
}

void rudp_aimd_resume_after_idle(struct rudp_aimd_cc *cc, uint64_t now_ms, double rto_ms,
                                 bool credit_limited)
{
    if (!cc->has_sent || credit_limited || now_ms < cc->last_data_send_ms)
        return;
    if ((double)(now_ms - cc->last_data_send_ms) >= rto_ms && cc->cwnd > RUDP_AIMD_INITIAL_WINDOW)
        cc->cwnd = RUDP_AIMD_INITIAL_WINDOW;
    cc->cwnd = clamp_window(cc->cwnd, cc->maximum_window);
}

static double packet_cost(size_t payload_bytes)
{
    double cost = (double)payload_bytes / RUDP_MAX_DATA_PAYLOAD;
    return cost > 0.0 ? cost : 1.0 / RUDP_MAX_DATA_PAYLOAD;
}

static double pacing_rtt(double srtt_ms)
{
    return srtt_ms > 0.0 ? srtt_ms : 1000.0;
}

static double available_credit(const struct rudp_pacer *pacer, uint64_t now_ms, double cwnd,
                               double srtt_ms)
{
    double credit = pacer->credit_packets;
    if (now_ms > pacer->updated_ms)
        credit += (double)(now_ms - pacer->updated_ms) * cwnd / pacing_rtt(srtt_ms);
    return credit > 2.0 ? 2.0 : credit;
}

void rudp_pacer_init(struct rudp_pacer *pacer, uint64_t now_ms)
{
    *pacer = (struct rudp_pacer){2.0, now_ms, true};
}

bool rudp_pacer_take(struct rudp_pacer *pacer, uint64_t now_ms, double cwnd, double srtt_ms,
                     size_t payload_bytes)
{
    const double cost = packet_cost(payload_bytes);
    if (!pacer->initialized)
        rudp_pacer_init(pacer, now_ms);
    pacer->credit_packets = available_credit(pacer, now_ms, cwnd, srtt_ms);
    pacer->updated_ms = now_ms;
    if (pacer->credit_packets + 1e-12 < cost)
        return false;
    pacer->credit_packets -= cost;
    return true;
}

uint64_t rudp_pacer_next_ms(const struct rudp_pacer *pacer, uint64_t now_ms, double cwnd,
                            double srtt_ms, size_t payload_bytes)
{
    const double missing =
        packet_cost(payload_bytes) - available_credit(pacer, now_ms, cwnd, srtt_ms);
    double wait_ms;
    uint64_t whole_ms;

    if (missing <= 0.0)
        return now_ms;
    wait_ms = missing * pacing_rtt(srtt_ms) / cwnd;
    whole_ms = (uint64_t)wait_ms;
    if ((double)whole_ms < wait_ms)
        whole_ms += 1U;
    return now_ms + whole_ms;
}
