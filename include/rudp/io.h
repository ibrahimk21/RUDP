#ifndef RUDP_IO_H
#define RUDP_IO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef ssize_t (*rudp_write_fn)(void *context, const uint8_t *bytes, size_t length);

int rudp_write_all(rudp_write_fn write_fn, void *context, const uint8_t *bytes, size_t length);

#endif
