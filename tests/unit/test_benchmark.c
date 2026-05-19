#include "rudp/benchmark.h"

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

    assert(stream != NULL);
    assert(rudp_benchmark_record_write(stream, &record) == 0);
    rewind(stream);
    length = fread(buffer, 1U, sizeof(buffer) - 1U, stream);
    buffer[length] = '\0';
    assert(strstr(buffer, "\"error\":\"quoted \\\"error\\\"\\u000a\"") != NULL);
    assert(strstr(buffer, "\"bytes\":42") != NULL);
    assert(strstr(buffer, "\"cc_requested\":\"bbr\"") != NULL);
    assert(fclose(stream) == 0);
    puts("benchmark record codec tests passed");
    return 0;
}
