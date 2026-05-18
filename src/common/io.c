#include "rudp/io.h"

#include <errno.h>

int rudp_write_all(rudp_write_fn write_fn, void *context, const uint8_t *bytes, size_t length)
{
    size_t offset = 0U;

    if (write_fn == NULL || (bytes == NULL && length != 0U)) {
        errno = EINVAL;
        return -1;
    }
    while (offset != length) {
        ssize_t count = write_fn(context, bytes + offset, length - offset);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0 || (size_t)count > length - offset) {
            if (count == 0) {
                errno = EIO;
            }
            return -1;
        }
        offset += (size_t)count;
    }
    return 0;
}
