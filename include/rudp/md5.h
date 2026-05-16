#ifndef RUDP_MD5_H
#define RUDP_MD5_H

#include <stddef.h>
#include <stdint.h>

struct rudp_md5 {
    uint32_t state[4];
    uint64_t length;
    uint8_t block[64];
    size_t block_length;
};

void rudp_md5_init(struct rudp_md5 *context);
void rudp_md5_update(struct rudp_md5 *context, const uint8_t *bytes, size_t length);
void rudp_md5_final(struct rudp_md5 *context, uint8_t digest[16]);
void rudp_md5_bytes(const uint8_t *bytes, size_t length, uint8_t digest[16]);

#endif
