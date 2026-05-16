#include "rudp/md5.h"

#include <string.h>

static uint32_t rotate_left(uint32_t value, uint32_t shift)
{
    return (value << shift) | (value >> (32U - shift));
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static void transform(struct rudp_md5 *context, const uint8_t block[64])
{
    static const uint32_t shifts[64] = {
        7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 5,  9,  14, 20, 5,  9,
        14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
        4,  11, 16, 23, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21,
    };
    static const uint32_t constants[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
        0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
        0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
        0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
        0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
        0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
        0xeb86d391,
    };
    uint32_t words[16];
    uint32_t a = context->state[0];
    uint32_t b = context->state[1];
    uint32_t c = context->state[2];
    uint32_t d = context->state[3];
    size_t index;

    for (index = 0; index < 16; ++index) {
        words[index] = read_le32(block + index * 4U);
    }
    for (index = 0; index < 64; ++index) {
        uint32_t function;
        size_t word;
        uint32_t saved_d = d;

        if (index < 16U) {
            function = (b & c) | ((~b) & d);
            word = index;
        } else if (index < 32U) {
            function = (d & b) | ((~d) & c);
            word = (5U * index + 1U) % 16U;
        } else if (index < 48U) {
            function = b ^ c ^ d;
            word = (3U * index + 5U) % 16U;
        } else {
            function = c ^ (b | (~d));
            word = (7U * index) % 16U;
        }
        d = c;
        c = b;
        b += rotate_left(a + function + constants[index] + words[word], shifts[index]);
        a = saved_d;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
}

void rudp_md5_init(struct rudp_md5 *context)
{
    memset(context, 0, sizeof(*context));
    context->state[0] = UINT32_C(0x67452301);
    context->state[1] = UINT32_C(0xefcdab89);
    context->state[2] = UINT32_C(0x98badcfe);
    context->state[3] = UINT32_C(0x10325476);
}

void rudp_md5_update(struct rudp_md5 *context, const uint8_t *bytes, size_t length)
{
    context->length += length;
    while (length != 0U) {
        size_t available = sizeof(context->block) - context->block_length;
        size_t take = length < available ? length : available;

        memcpy(context->block + context->block_length, bytes, take);
        context->block_length += take;
        bytes += take;
        length -= take;
        if (context->block_length == sizeof(context->block)) {
            transform(context, context->block);
            context->block_length = 0U;
        }
    }
}

void rudp_md5_final(struct rudp_md5 *context, uint8_t digest[16])
{
    uint64_t bit_length = context->length * 8U;
    uint8_t padding[72] = {0x80U};
    size_t padding_length =
        context->block_length < 56U ? 56U - context->block_length : 120U - context->block_length;
    size_t index;

    for (index = 0; index < 8U; ++index) {
        padding[padding_length + index] = (uint8_t)(bit_length >> (8U * index));
    }
    rudp_md5_update(context, padding, padding_length + 8U);
    for (index = 0; index < 4U; ++index) {
        write_le32(digest + index * 4U, context->state[index]);
    }
}

void rudp_md5_bytes(const uint8_t *bytes, size_t length, uint8_t digest[16])
{
    struct rudp_md5 context;

    rudp_md5_init(&context);
    rudp_md5_update(&context, bytes, length);
    rudp_md5_final(&context, digest);
}
