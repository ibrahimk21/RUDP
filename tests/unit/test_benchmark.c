#include "rudp/benchmark.h"
#include "rudp/record.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const struct rudp_benchmark_record record = {
        .tool = "tcp_ref",
        .role = "sender",
        .status = "failure",
        .stage = "test",
        .error = "quoted \"error\"\n",
        .cc_requested = "bbr",
        .cc_actual = "",
        .bytes = 42U,
        .tcp_snd_cwnd = 10U,
        .socket_send_buffer = 4096,
        .socket_receive_buffer = 8192,
    };
    char buffer[2048];
    FILE *stream = tmpfile();
    size_t length;
    uint8_t generated[RUDP_BENCHMARK_RECORD_SIZE];
    uint64_t timestamp;
    struct rudp_benchmark_clock first;
    struct rudp_benchmark_clock second;

    assert(stream != NULL);
    assert(rudp_benchmark_record_write(stream, &record) == 0);
    rewind(stream);
    length = fread(buffer, 1U, sizeof(buffer) - 1U, stream);
    buffer[length] = '\0';
    assert(strstr(buffer, "\"error\":\"quoted \\\"error\\\"\\u000a\"") != NULL);
    assert(strstr(buffer, "\"bytes\":42") != NULL);
    assert(strstr(buffer, "\"cc_requested\":\"bbr\"") != NULL);
    if (fclose(stream) != 0)
        return 1;
    stream = NULL;
    rudp_record_make(generated, 77U, 123456789U);
    assert(rudp_record_id(generated) == 77U);
    assert(rudp_record_validate(generated, 77U, &timestamp));
    assert(timestamp == 123456789U);
    generated[500U] ^= 1U;
    assert(!rudp_record_validate(generated, 77U, NULL));
    assert(rudp_benchmark_clock_read(&first) == 0);
    assert(rudp_benchmark_clock_read(&second) == 0);
    assert(second.monotonic_ns >= first.monotonic_ns);
    assert(second.user_cpu_ns >= first.user_cpu_ns);
    assert(second.system_cpu_ns >= first.system_cpu_ns);
    puts("benchmark record codec tests passed");
    return 0;
}
