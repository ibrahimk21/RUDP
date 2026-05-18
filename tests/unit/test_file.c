#include "rudp/file.h"
#include "rudp/md5.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_exact(const char *path, const uint8_t *bytes, size_t length)
{
    FILE *stream = fopen(path, "wb");
    size_t written;
    int close_result;

    if (stream == NULL) {
        abort();
    }
    written = fwrite(bytes, 1U, length, stream);
    close_result = fclose(stream);
    assert(written == length);
    assert(close_result == 0);
}

int main(void)
{
    static const uint8_t content[] = "checked file output";
    char directory[] = "/tmp/rudp-file-test.XXXXXX";
    char source_path[128];
    char output_path[128];
    struct rudp_source_file source;
    struct rudp_output_file output;
    uint8_t digest[16];
    uint8_t readback[sizeof(content)];
    FILE *stream;
    size_t read_length;
    int close_result;

    assert(mkdtemp(directory) != NULL);
    assert(snprintf(source_path, sizeof(source_path), "%s/source", directory) > 0);
    assert(snprintf(output_path, sizeof(output_path), "%s/output", directory) > 0);
    write_exact(source_path, content, sizeof(content));
    assert(rudp_source_load(&source, source_path) == 0);
    assert(source.length == sizeof(content));
    assert(memcmp(source.bytes, content, sizeof(content)) == 0);
    assert(rudp_source_verify(&source) == 0);

    assert(rudp_output_open(&output, output_path) == 0);
    assert(rudp_output_append(&output, content, 7U) == 0);
    assert(rudp_output_append(&output, content + 7U, sizeof(content) - 7U) == 0);
    rudp_md5_bytes(content, sizeof(content), digest);
    assert(rudp_output_finish(&output, sizeof(content), digest) == 0);
    assert(output.committed);
    stream = fopen(output_path, "rb");
    if (stream == NULL) {
        abort();
    }
    read_length = fread(readback, 1U, sizeof(readback), stream);
    close_result = fclose(stream);
    assert(read_length == sizeof(readback));
    assert(close_result == 0);
    assert(memcmp(readback, content, sizeof(content)) == 0);
    rudp_output_abort(&output);

    errno = 0;
    assert(rudp_output_open(&output, output_path) != 0);
    assert(errno == EEXIST);
    write_exact(source_path, (const uint8_t *)"changed", 7U);
    assert(rudp_source_verify(&source) != 0);
    rudp_source_release(&source);

    assert(unlink(source_path) == 0);
    assert(unlink(output_path) == 0);
    assert(rmdir(directory) == 0);
    puts("file boundary tests passed");
    return 0;
}
