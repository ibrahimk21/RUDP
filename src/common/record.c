#include "rudp/record.h"

#include <stddef.h>

static void put_u64(uint8_t *output, uint64_t value)
{
    unsigned int index;
    for (index = 0U; index < 8U; ++index)
        output[index] = (uint8_t)(value >> (56U - index * 8U));
}

static uint64_t get_u64(const uint8_t *input)
{
    uint64_t value = 0U;
    unsigned int index;
    for (index = 0U; index < 8U; ++index)
        value = (value << 8U) | input[index];
    return value;
}

void rudp_record_make(uint8_t record[RUDP_BENCHMARK_RECORD_SIZE], uint64_t id,
                      uint64_t offer_time_ns)
{
    size_t index;
    put_u64(record, id);
    put_u64(record + 8U, offer_time_ns);
    for (index = RUDP_BENCHMARK_RECORD_HEADER_SIZE; index < RUDP_BENCHMARK_RECORD_SIZE; ++index)
        record[index] = (uint8_t)(id * 31U + index * 17U);
}

uint64_t rudp_record_id(const uint8_t record[RUDP_BENCHMARK_RECORD_SIZE])
{
    return get_u64(record);
}

bool rudp_record_validate(const uint8_t record[RUDP_BENCHMARK_RECORD_SIZE], uint64_t expected_id,
                          uint64_t *offer_time_ns)
{
    size_t index;
    if (get_u64(record) != expected_id)
        return false;
    for (index = RUDP_BENCHMARK_RECORD_HEADER_SIZE; index < RUDP_BENCHMARK_RECORD_SIZE; ++index) {
        if (record[index] != (uint8_t)(expected_id * 31U + index * 17U))
            return false;
    }
    if (offer_time_ns != NULL)
        *offer_time_ns = get_u64(record + 8U);
    return true;
}
