#include "rudp/md5.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check(const char *input, const uint8_t expected[16])
{
    uint8_t actual[16];
    struct rudp_md5 split;
    size_t length = strlen(input);

    rudp_md5_bytes((const uint8_t *)input, length, actual);
    assert(memcmp(actual, expected, 16U) == 0);
    rudp_md5_init(&split);
    rudp_md5_update(&split, (const uint8_t *)input, length / 2U);
    rudp_md5_update(&split, (const uint8_t *)input + length / 2U, length - length / 2U);
    rudp_md5_final(&split, actual);
    assert(memcmp(actual, expected, 16U) == 0);
}

int main(void)
{
    static const uint8_t empty[16] = {0xd4, 0x1d, 0x8c, 0xd9, 0x8f, 0x00, 0xb2, 0x04,
                                      0xe9, 0x80, 0x09, 0x98, 0xec, 0xf8, 0x42, 0x7e};
    static const uint8_t abc[16] = {0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
                                    0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72};
    static const uint8_t alphabet[16] = {0xc3, 0xfc, 0xd3, 0xd7, 0x61, 0x92, 0xe4, 0x00,
                                         0x7d, 0xfb, 0x49, 0x6c, 0xca, 0x67, 0xe1, 0x3b};

    check("", empty);
    check("abc", abc);
    check("abcdefghijklmnopqrstuvwxyz", alphabet);
    puts("MD5 standard vectors passed");
    return 0;
}
