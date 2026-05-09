#ifndef RUDP_CONGESTION_H
#define RUDP_CONGESTION_H

#include <stdint.h>

struct rudp_fixed_cc {
    uint32_t window;
};

void rudp_fixed_cc_init(struct rudp_fixed_cc *cc, uint32_t window);
uint32_t rudp_fixed_cc_window(const struct rudp_fixed_cc *cc);

#endif
