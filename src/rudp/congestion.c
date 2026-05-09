#include "rudp/congestion.h"

#include "rudp/window.h"

void rudp_fixed_cc_init(struct rudp_fixed_cc *cc, uint32_t window)
{
    cc->window = window == 0U ? 1U : window;
    if (cc->window > RUDP_WINDOW_CAPACITY) {
        cc->window = RUDP_WINDOW_CAPACITY;
    }
}

uint32_t rudp_fixed_cc_window(const struct rudp_fixed_cc *cc)
{
    return cc->window;
}
