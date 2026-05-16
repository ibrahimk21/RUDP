#include "rudp/file.h"

#include "rudp/session.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool same_identity(const struct stat *left, const struct stat *right)
{
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino &&
           left->st_size == right->st_size && left->st_mtim.tv_sec == right->st_mtim.tv_sec &&
           left->st_mtim.tv_nsec == right->st_mtim.tv_nsec;
}

static int hash_path(const char *path, struct stat *identity, uint8_t digest[16],
                     uint8_t **contents, size_t *content_length)
{
    FILE *stream = fopen(path, "rb");
    struct rudp_md5 md5;
    uint8_t buffer[65536];
    size_t offset = 0U;
    uint8_t *bytes = NULL;

    if (stream == NULL || fstat(fileno(stream), identity) != 0 || !S_ISREG(identity->st_mode) ||
        identity->st_size < 0 || (uint64_t)identity->st_size > RUDP_MAX_TRANSFER_LENGTH ||
        (uintmax_t)identity->st_size > SIZE_MAX) {
        if (stream != NULL) {
            (void)fclose(stream);
        }
        errno = EFBIG;
        return -1;
    }
    if (contents != NULL) {
        bytes = malloc(identity->st_size == 0 ? 1U : (size_t)identity->st_size);
        if (bytes == NULL) {
            (void)fclose(stream);
            return -1;
        }
    }
    rudp_md5_init(&md5);
    for (;;) {
        size_t count = fread(buffer, 1U, sizeof(buffer), stream);

        if (count != 0U) {
            rudp_md5_update(&md5, buffer, count);
            if (bytes != NULL) {
                memcpy(bytes + offset, buffer, count);
            }
            offset += count;
        }
        if (count != sizeof(buffer)) {
            if (ferror(stream) != 0 || fclose(stream) != 0 || offset != (size_t)identity->st_size) {
                free(bytes);
                return -1;
            }
            break;
        }
    }
    rudp_md5_final(&md5, digest);
    if (contents != NULL) {
        *contents = bytes;
        *content_length = offset;
    }
    return 0;
}

int rudp_source_load(struct rudp_source_file *source, const char *path)
{
    if (source == NULL || path == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(source, 0, sizeof(*source));
    source->path = strdup(path);
    if (source->path == NULL ||
        hash_path(path, &source->identity, source->digest, &source->bytes, &source->length) != 0) {
        rudp_source_release(source);
        return -1;
    }
    return 0;
}

int rudp_source_verify(const struct rudp_source_file *source)
{
    struct stat identity;
    uint8_t digest[16];

    if (source == NULL || source->path == NULL ||
        hash_path(source->path, &identity, digest, NULL, NULL) != 0) {
        return -1;
    }
    if (!same_identity(&source->identity, &identity) ||
        memcmp(source->digest, digest, sizeof(digest)) != 0) {
        errno = ESTALE;
        return -1;
    }
    return 0;
}

void rudp_source_release(struct rudp_source_file *source)
{
    if (source != NULL) {
        free(source->bytes);
        free(source->path);
        memset(source, 0, sizeof(*source));
    }
}

int rudp_output_open(struct rudp_output_file *output, const char *destination)
{
    size_t length;

    if (output == NULL || destination == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(output, 0, sizeof(*output));
    output->fd = -1;
    if (access(destination, F_OK) == 0) {
        errno = EEXIST;
        return -1;
    }
    length = strlen(destination) + sizeof(".rudp-partial.XXXXXX");
    output->destination_path = strdup(destination);
    output->temporary_path = malloc(length);
    if (output->destination_path == NULL || output->temporary_path == NULL) {
        rudp_output_abort(output);
        return -1;
    }
    (void)snprintf(output->temporary_path, length, "%s.rudp-partial.XXXXXX", destination);
    output->fd = mkstemp(output->temporary_path);
    if (output->fd < 0) {
        rudp_output_abort(output);
        return -1;
    }
    output->stream = fdopen(output->fd, "wb");
    if (output->stream == NULL) {
        rudp_output_abort(output);
        return -1;
    }
    rudp_md5_init(&output->md5);
    return 0;
}

int rudp_output_append(void *context, const uint8_t *bytes, size_t length)
{
    struct rudp_output_file *output = context;
    size_t written = 0U;

    if (output == NULL || output->stream == NULL ||
        output->length + length > RUDP_MAX_TRANSFER_LENGTH) {
        errno = EFBIG;
        return -1;
    }
    while (written != length) {
        size_t count = fwrite(bytes + written, 1U, length - written, output->stream);

        if (count == 0U) {
            return -1;
        }
        rudp_md5_update(&output->md5, bytes + written, count);
        output->length += count;
        written += count;
    }
    return 0;
}

int rudp_output_finish(void *context, uint64_t length, const uint8_t digest[16])
{
    struct rudp_output_file *output = context;
    struct rudp_md5 final_md5;
    uint8_t actual[16];

    if (output == NULL || output->stream == NULL || length != output->length) {
        errno = EIO;
        return -1;
    }
    final_md5 = output->md5;
    rudp_md5_final(&final_md5, actual);
    if (memcmp(actual, digest, sizeof(actual)) != 0 || fflush(output->stream) != 0 ||
        fsync(output->fd) != 0 || fclose(output->stream) != 0) {
        output->stream = NULL;
        output->fd = -1;
        errno = EIO;
        return -1;
    }
    output->stream = NULL;
    output->fd = -1;
    if (link(output->temporary_path, output->destination_path) != 0) {
        return -1;
    }
    if (unlink(output->temporary_path) != 0) {
        (void)unlink(output->destination_path);
        return -1;
    }
    output->committed = true;
    return 0;
}

void rudp_output_abort(struct rudp_output_file *output)
{
    if (output == NULL) {
        return;
    }
    if (output->stream != NULL) {
        (void)fclose(output->stream);
    } else if (output->fd >= 0) {
        (void)close(output->fd);
    }
    if (!output->committed && output->temporary_path != NULL) {
        (void)unlink(output->temporary_path);
    }
    free(output->temporary_path);
    free(output->destination_path);
    memset(output, 0, sizeof(*output));
    output->fd = -1;
}
