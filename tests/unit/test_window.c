#include "rudp/congestion.h"
#include "rudp/window.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    static struct rudp_receive_window window;
    struct rudp_sack_block sacks[RUDP_MAX_SACK_BLOCKS];
    static const uint8_t byte = 1U;

    assert(rudp_seq_after(0U, UINT32_MAX));
    assert(rudp_seq_before(UINT32_MAX, 0U));
    assert(!rudp_seq_after(UINT32_C(0x80000000), 0U));
    assert(rudp_seq_in_window(0U, UINT32_MAX - 1U, 2U));

    rudp_receive_window_init(&window, UINT32_MAX - 1U);
    assert(rudp_receive_window_insert(&window, 0U, &byte, 1U));
    assert(rudp_receive_window_insert(&window, 1U, &byte, 1U));
    assert(window.expected == UINT32_MAX - 1U);
    assert(rudp_receive_window_sacks(&window, sacks) == 1U);
    assert(sacks[0].start == 0U && sacks[0].end == 2U);
    assert(rudp_receive_window_insert(&window, UINT32_MAX - 1U, &byte, 1U));
    assert(window.expected == UINT32_MAX);
    assert(rudp_receive_window_insert(&window, UINT32_MAX, &byte, 1U));
    assert(window.expected == 2U);
    assert(!rudp_receive_window_insert(&window, window.consumed + RUDP_WINDOW_CAPACITY, &byte,
                                       1U));
    assert(rudp_receive_window_consume(&window, UINT32_MAX - 1U));
    assert(rudp_receive_window_limit(&window) == UINT32_MAX + RUDP_WINDOW_CAPACITY);

    {
        static struct rudp_send_scoreboard scoreboard;
        const struct rudp_sack_block first = {.start = 1U, .end = 2U};
        const struct rudp_sack_block second = {.start = 2U, .end = 3U};
        const struct rudp_sack_block third = {.start = 3U, .end = 4U};
        struct rudp_ack_update update;

        rudp_send_scoreboard_init(&scoreboard, 0U, 8U);
        assert(rudp_send_scoreboard_track(&scoreboard, 0U, &byte, 1U));
        assert(rudp_send_scoreboard_track(&scoreboard, 1U, &byte, 1U));
        assert(rudp_send_scoreboard_track(&scoreboard, 2U, &byte, 1U));
        assert(rudp_send_scoreboard_track(&scoreboard, 3U, &byte, 1U));
        assert(rudp_send_scoreboard_apply_ack(&scoreboard, 0U, 8U, &first, 1U, &update) ==
               RUDP_ACK_CHANGED);
        assert(rudp_send_scoreboard_apply_ack(&scoreboard, 0U, 8U, &second, 1U, &update) ==
               RUDP_ACK_CHANGED);
        assert(rudp_send_scoreboard_apply_ack(&scoreboard, 0U, 8U, &third, 1U, &update) ==
               RUDP_ACK_CHANGED);
        assert(rudp_send_scoreboard_next_fast_retransmit(&scoreboard)->sequence == 0U);
    }
    {
        struct rudp_packet ack;
        struct rudp_fixed_cc cc;

        rudp_receive_window_make_ack(&window, 1U, 2U, &ack);
        assert(ack.type == RUDP_PACKET_ACK);
        assert(ack.ack == window.expected);
        assert(ack.receive_limit == rudp_receive_window_limit(&window));
        rudp_fixed_cc_init(&cc, 0U);
        assert(rudp_fixed_cc_window(&cc) == 1U);
        rudp_fixed_cc_init(&cc, RUDP_WINDOW_CAPACITY + 1U);
        assert(rudp_fixed_cc_window(&cc) == RUDP_WINDOW_CAPACITY);
    }
    puts("window tests passed");
    return 0;
}
