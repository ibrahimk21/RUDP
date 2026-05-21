#include "rudp/benchmark.h"

#include <inttypes.h>
#include <sys/resource.h>
#include <time.h>

static uint64_t timeval_ns(const struct timeval *value)
{
    return (uint64_t)value->tv_sec * UINT64_C(1000000000) + (uint64_t)value->tv_usec * 1000U;
}

int rudp_benchmark_clock_read(struct rudp_benchmark_clock *clock)
{
    struct timespec monotonic;
    struct rusage usage;

    if (clock == NULL || clock_gettime(CLOCK_MONOTONIC, &monotonic) != 0 ||
        getrusage(RUSAGE_SELF, &usage) != 0)
        return -1;
    clock->monotonic_ns =
        (uint64_t)monotonic.tv_sec * UINT64_C(1000000000) + (uint64_t)monotonic.tv_nsec;
    clock->user_cpu_ns = timeval_ns(&usage.ru_utime);
    clock->system_cpu_ns = timeval_ns(&usage.ru_stime);
    return 0;
}

static int json_string(FILE *stream, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)(text == NULL ? "" : text);

    if (fputc('"', stream) == EOF)
        return -1;
    while (*cursor != '\0') {
        if (*cursor == '"' || *cursor == '\\') {
            if (fputc('\\', stream) == EOF || fputc(*cursor, stream) == EOF)
                return -1;
        } else if (*cursor < 0x20U) {
            if (fprintf(stream, "\\u%04x", (unsigned int)*cursor) < 0)
                return -1;
        } else if (fputc(*cursor, stream) == EOF) {
            return -1;
        }
        cursor += 1;
    }
    return fputc('"', stream) == EOF ? -1 : 0;
}

static int field(FILE *stream, const char *name, const char *value)
{
    return fprintf(stream, "\"") < 0 || fputs(name, stream) == EOF || fputs("\":", stream) == EOF ||
                   json_string(stream, value) != 0
               ? -1
               : 0;
}

int rudp_benchmark_record_write(FILE *stream, const struct rudp_benchmark_record *record)
{
    if (stream == NULL || record == NULL || fputc('{', stream) == EOF ||
        field(stream, "tool", record->tool) != 0 || fputc(',', stream) == EOF ||
        field(stream, "status", record->status) != 0 || fputc(',', stream) == EOF ||
        field(stream, "role", record->role) != 0 || fputc(',', stream) == EOF ||
        field(stream, "stage", record->stage) != 0 || fputc(',', stream) == EOF ||
        field(stream, "error", record->error) != 0 || fputc(',', stream) == EOF ||
        field(stream, "cc_requested", record->cc_requested) != 0 || fputc(',', stream) == EOF ||
        field(stream, "cc_actual", record->cc_actual) != 0)
        return -1;
    if (fprintf(stream,
                ",\"bytes\":%" PRIu64 ",\"packets_sent\":%" PRIu64 ",\"packets_received\":%" PRIu64
                ",\"malformed_packets\":%" PRIu64 ",\"unique_bytes\":%" PRIu64
                ",\"duplicate_records\":%" PRIu64 ",\"missing_records\":%" PRIu64
                ",\"started_ns\":%" PRIu64 ",\"ready_ns\":%" PRIu64 ",\"first_byte_ns\":%" PRIu64
                ",\"ended_ns\":%" PRIu64 ",\"user_cpu_ns\":%" PRIu64 ",\"system_cpu_ns\":%" PRIu64
                ",\"fast_retransmits\":%u,\"timeout_retransmits\":%u"
                ",\"clean_rtt_samples\":%u,\"suppressed_rtt_samples\":%u"
                ",\"tcp_snd_cwnd\":%u,\"tcp_snd_mss\":%u,\"tcp_rtt_us\":%u,\"tcp_retransmits\":%u"
                ",\"socket_send_buffer\":%d,\"socket_receive_buffer\":%d}\n",
                record->bytes, record->packets_sent, record->packets_received,
                record->malformed_packets, record->unique_bytes, record->duplicate_records,
                record->missing_records, record->started_ns, record->ready_ns,
                record->first_byte_ns, record->ended_ns, record->user_cpu_ns, record->system_cpu_ns,
                record->fast_retransmits, record->timeout_retransmits, record->clean_rtt_samples,
                record->suppressed_rtt_samples, record->tcp_snd_cwnd, record->tcp_snd_mss,
                record->tcp_rtt_us, record->tcp_retransmits, record->socket_send_buffer,
                record->socket_receive_buffer) < 0)
        return -1;
    return fflush(stream);
}
