#ifndef RUDP_FILE_H
#define RUDP_FILE_H

#include "rudp/md5.h"
#include "rudp/stopwait.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

struct rudp_source_file {
    uint8_t *bytes;
    size_t length;
    uint8_t digest[16];
    struct stat identity;
    char *path;
};

struct rudp_output_file {
    FILE *stream;
    int fd;
    char *temporary_path;
    char *destination_path;
    struct rudp_md5 md5;
    uint64_t length;
    bool committed;
};

int rudp_source_load(struct rudp_source_file *source, const char *path);
int rudp_source_verify(const struct rudp_source_file *source);
void rudp_source_release(struct rudp_source_file *source);

int rudp_output_open(struct rudp_output_file *output, const char *destination);
int rudp_output_append(void *context, const uint8_t *bytes, size_t length);
int rudp_output_finish(void *context, uint64_t length, const uint8_t digest[16]);
void rudp_output_abort(struct rudp_output_file *output);

#endif
