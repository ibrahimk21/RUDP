#ifndef RUDP_RECORD_H
#define RUDP_RECORD_H

#include <stdbool.h>
#include <stdint.h>

#define RUDP_BENCHMARK_RECORD_SIZE 1024U
#define RUDP_BENCHMARK_RECORD_HEADER_SIZE 16U

void rudp_record_make(uint8_t record[RUDP_BENCHMARK_RECORD_SIZE], uint64_t id,
                      uint64_t offer_time_ns);
uint64_t rudp_record_id(const uint8_t record[RUDP_BENCHMARK_RECORD_SIZE]);
bool rudp_record_validate(const uint8_t record[RUDP_BENCHMARK_RECORD_SIZE], uint64_t expected_id,
                          uint64_t *offer_time_ns);

#endif
