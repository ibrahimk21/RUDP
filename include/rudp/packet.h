#ifndef RUDP_PACKET_H
#define RUDP_PACKET_H

#include <stddef.h>
#include <stdint.h>

#define RUDP_VERSION 1U
#define RUDP_HEADER_SIZE 36U
#define RUDP_MAX_SACK_BLOCKS 4U
#define RUDP_MAX_DATA_PAYLOAD 1024U
#define RUDP_MAX_DATAGRAM_SIZE \
  (RUDP_HEADER_SIZE + (RUDP_MAX_SACK_BLOCKS * 8U) + RUDP_MAX_DATA_PAYLOAD)

enum rudp_packet_type {
  RUDP_PACKET_SYN = 1,
  RUDP_PACKET_SYN_ACK = 2,
  RUDP_PACKET_OPEN = 3,
  RUDP_PACKET_OPEN_ACK = 4,
  RUDP_PACKET_DATA = 5,
  RUDP_PACKET_ACK = 6,
  RUDP_PACKET_PROBE = 7,
  RUDP_PACKET_FIN = 8,
  RUDP_PACKET_FIN_ACK = 9,
  RUDP_PACKET_ABORT = 10,
};

enum rudp_packet_error {
  RUDP_PACKET_OK = 0,
  RUDP_PACKET_ERR_ARGUMENT,
  RUDP_PACKET_ERR_CAPACITY,
  RUDP_PACKET_ERR_TRUNCATED,
  RUDP_PACKET_ERR_VERSION,
  RUDP_PACKET_ERR_TYPE,
  RUDP_PACKET_ERR_RESERVED,
  RUDP_PACKET_ERR_SACK_COUNT,
  RUDP_PACKET_ERR_LENGTH,
  RUDP_PACKET_ERR_CHECKSUM,
  RUDP_PACKET_ERR_FIELD,
  RUDP_PACKET_ERR_SACK_RANGE,
};

struct rudp_sack_block {
  uint32_t start;
  uint32_t end;
};

struct rudp_packet {
  enum rudp_packet_type type;
  uint64_t client_nonce;
  uint64_t server_nonce;
  uint32_t seq;
  uint32_t ack;
  uint32_t receive_limit;
  uint8_t sack_count;
  struct rudp_sack_block sacks[RUDP_MAX_SACK_BLOCKS];
  const uint8_t *data;
  uint16_t data_length;
  uint64_t transfer_length;
  uint8_t digest[16];
  uint32_t abort_code;
};

uint16_t rudp_internet_checksum(const uint8_t *bytes, size_t length);

enum rudp_packet_error rudp_packet_encode(const struct rudp_packet *packet,
                                          uint8_t *output,
                                          size_t output_capacity,
                                          size_t *output_length);

enum rudp_packet_error rudp_packet_decode(struct rudp_packet *packet,
                                          const uint8_t *input,
                                          size_t input_length);

const char *rudp_packet_error_string(enum rudp_packet_error error);

#endif
