#include "rudp/window.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct rudp_receive_window window;
    struct rudp_sack_block sacks[RUDP_MAX_SACK_BLOCKS];

    assert(rudp_seq_after(0U, UINT32_MAX));
    assert(rudp_seq_before(UINT32_MAX, 0U));
    assert(!rudp_seq_after(UINT32_C(0x80000000), 0U));
    assert(rudp_seq_in_window(0U, UINT32_MAX - 1U, 2U));

    rudp_receive_window_init(&window, UINT32_MAX - 1U);
    assert(rudp_receive_window_insert(&window, 0U, 1U));
    assert(rudp_receive_window_insert(&window, 1U, 1U));
    assert(window.expected == UINT32_MAX - 1U);
    assert(rudp_receive_window_sacks(&window, sacks) == 1U);
    assert(sacks[0].start == 0U && sacks[0].end == 2U);
    assert(rudp_receive_window_insert(&window, UINT32_MAX - 1U, 1U));
    assert(window.expected == UINT32_MAX);
    assert(rudp_receive_window_insert(&window, UINT32_MAX, 1U));
    assert(window.expected == 2U);
    assert(!rudp_receive_window_insert(&window, window.consumed + RUDP_WINDOW_CAPACITY, 1U));
    puts("window tests passed");
    return 0;
}
