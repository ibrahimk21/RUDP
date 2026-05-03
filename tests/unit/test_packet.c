#include "rudp/packet.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t digest[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                   0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

static void checksum(uint8_t *bytes, size_t length)
{
    bytes[34] = 0U;
    bytes[35] = 0U;
    bytes[34] = (uint8_t)(rudp_internet_checksum(bytes, length) >> 8U);
    bytes[35] = (uint8_t)rudp_internet_checksum(bytes, length);
}

static struct rudp_packet packet_for(enum rudp_packet_type type)
{
    static const uint8_t data[] = {0xde, 0xad, 0xbe, 0xef, 0x01};
    struct rudp_packet packet;

    memset(&packet, 0, sizeof(packet));
    packet.type = type;
    packet.client_nonce = UINT64_C(0x0102030405060708);
    packet.server_nonce = UINT64_C(0x1112131415161718);
    packet.seq = 7U;
    packet.ack = 5U;
    packet.receive_limit = 8197U;
    packet.data = data;
    packet.data_length = (uint16_t)sizeof(data);
    packet.transfer_length = UINT64_C(0x1122334455667788);
    memcpy(packet.digest, digest, sizeof(digest));
    packet.abort_code = UINT32_C(0xaabbccdd);

    switch (type) {
    case RUDP_PACKET_SYN:
        packet.server_nonce = 0U;
        packet.seq = 0U;
        packet.ack = 0U;
        packet.receive_limit = 0U;
        break;
    case RUDP_PACKET_SYN_ACK:
        packet.seq = 0U;
        packet.ack = 0U;
        packet.receive_limit = 0U;
        break;
    case RUDP_PACKET_OPEN:
    case RUDP_PACKET_PROBE:
    case RUDP_PACKET_ABORT:
        packet.seq = 0U;
        packet.ack = 0U;
        packet.receive_limit = 0U;
        break;
    case RUDP_PACKET_OPEN_ACK:
        packet.seq = 0U;
        break;
    case RUDP_PACKET_ACK:
        packet.seq = 0U;
        break;
    case RUDP_PACKET_DATA:
        packet.ack = 0U;
        packet.receive_limit = 0U;
        break;
    case RUDP_PACKET_FIN:
        packet.ack = 0U;
        packet.receive_limit = 0U;
        break;
    case RUDP_PACKET_FIN_ACK:
        break;
    }
    return packet;
}

static void test_golden_syn(void)
{
    static const uint8_t expected[] = {
        0x01, 0x01, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x18, 0xa5, 0x3d, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    struct rudp_packet packet = packet_for(RUDP_PACKET_SYN);
    uint8_t encoded[RUDP_MAX_DATAGRAM_SIZE];
    size_t length;

    assert(rudp_packet_encode(&packet, encoded, sizeof(encoded), &length) == RUDP_PACKET_OK);
    assert(length == sizeof(expected));
    assert(memcmp(encoded, expected, sizeof(expected)) == 0);
}

static void test_packet_types(void)
{
    enum rudp_packet_type type;

    for (type = RUDP_PACKET_SYN; type <= RUDP_PACKET_ABORT; ++type) {
        struct rudp_packet packet = packet_for(type);
        struct rudp_packet decoded;
        uint8_t encoded[RUDP_MAX_DATAGRAM_SIZE];
        size_t length;

        assert(rudp_packet_encode(&packet, encoded, sizeof(encoded), &length) == RUDP_PACKET_OK);
        assert(rudp_packet_decode(&decoded, encoded, length) == RUDP_PACKET_OK);
        assert(decoded.type == type);
        assert(decoded.client_nonce == packet.client_nonce);
        assert(decoded.server_nonce == packet.server_nonce);
        if (type == RUDP_PACKET_DATA) {
            assert(decoded.data_length == packet.data_length);
            assert(memcmp(decoded.data, packet.data, packet.data_length) == 0);
        }
        if (type == RUDP_PACKET_SYN || type == RUDP_PACKET_SYN_ACK || type == RUDP_PACKET_FIN ||
            type == RUDP_PACKET_FIN_ACK) {
            assert(decoded.transfer_length == packet.transfer_length);
            assert(memcmp(decoded.digest, digest, sizeof(digest)) == 0);
        }
    }
}

static void test_sacks(void)
{
    const uint8_t sack_counts[] = {0U, 1U, 4U};
    size_t count_index;

    for (count_index = 0; count_index < sizeof(sack_counts); ++count_index) {
        struct rudp_packet packet = packet_for(RUDP_PACKET_ACK);
        struct rudp_packet decoded;
        uint8_t encoded[RUDP_MAX_DATAGRAM_SIZE];
        uint8_t index;
        size_t length;

        packet.sack_count = sack_counts[count_index];
        for (index = 0U; index < packet.sack_count; ++index) {
            packet.sacks[index].start = packet.ack + 1U + (uint32_t)(index * 2U);
            packet.sacks[index].end = packet.sacks[index].start + 1U;
        }
        assert(rudp_packet_encode(&packet, encoded, sizeof(encoded), &length) == RUDP_PACKET_OK);
        assert(rudp_packet_decode(&decoded, encoded, length) == RUDP_PACKET_OK);
        assert(decoded.sack_count == packet.sack_count);
        assert(memcmp(decoded.sacks, packet.sacks,
                      (size_t)packet.sack_count * sizeof(packet.sacks[0])) == 0);
    }
}

static void test_rejections(void)
{
    struct rudp_packet packet = packet_for(RUDP_PACKET_ACK);
    struct rudp_packet decoded;
    uint8_t encoded[RUDP_MAX_DATAGRAM_SIZE];
    uint8_t unaligned[RUDP_MAX_DATAGRAM_SIZE + 1U];
    size_t length;

    assert(rudp_packet_encode(&packet, encoded, sizeof(encoded), &length) == RUDP_PACKET_OK);
    assert(rudp_packet_decode(&decoded, encoded, RUDP_HEADER_SIZE - 1U) ==
           RUDP_PACKET_ERR_TRUNCATED);

    encoded[0] = 2U;
    checksum(encoded, length);
    assert(rudp_packet_decode(&decoded, encoded, length) == RUDP_PACKET_ERR_VERSION);
    encoded[0] = RUDP_VERSION;
    encoded[1] = 99U;
    checksum(encoded, length);
    assert(rudp_packet_decode(&decoded, encoded, length) == RUDP_PACKET_ERR_TYPE);
    encoded[1] = RUDP_PACKET_ACK;
    encoded[3] = 1U;
    checksum(encoded, length);
    assert(rudp_packet_decode(&decoded, encoded, length) == RUDP_PACKET_ERR_RESERVED);

    encoded[3] = 0U;
    encoded[2] = 5U;
    checksum(encoded, length);
    assert(rudp_packet_decode(&decoded, encoded, length) == RUDP_PACKET_ERR_SACK_COUNT);

    encoded[2] = 0U;
    encoded[32] = 0U;
    encoded[33] = 1U;
    checksum(encoded, length);
    assert(rudp_packet_decode(&decoded, encoded, length) == RUDP_PACKET_ERR_LENGTH);

    assert(rudp_packet_encode(&packet, encoded, sizeof(encoded), &length) == RUDP_PACKET_OK);
    encoded[4] ^= 0x80U;
    assert(rudp_packet_decode(&decoded, encoded, length) == RUDP_PACKET_ERR_CHECKSUM);

    assert(rudp_packet_encode(&packet, encoded, sizeof(encoded), &length) == RUDP_PACKET_OK);
    memcpy(unaligned + 1U, encoded, length);
    assert(rudp_packet_decode(&decoded, unaligned + 1U, length) == RUDP_PACKET_OK);
}

static void test_sack_ranges_and_checksum(void)
{
    struct rudp_packet packet = packet_for(RUDP_PACKET_ACK);
    struct rudp_packet decoded;
    uint8_t encoded[RUDP_MAX_DATAGRAM_SIZE];
    const uint8_t odd_bytes[] = {0x01U, 0x02U, 0x03U};
    size_t length;

    packet.sack_count = 1U;
    packet.sacks[0].start = packet.ack;
    packet.sacks[0].end = packet.ack + 1U;
    assert(rudp_packet_encode(&packet, encoded, sizeof(encoded), &length) ==
           RUDP_PACKET_ERR_SACK_RANGE);

    packet.sacks[0].start = packet.ack + 2U;
    packet.sacks[0].end = packet.ack + 1U;
    assert(rudp_packet_encode(&packet, encoded, sizeof(encoded), &length) ==
           RUDP_PACKET_ERR_SACK_RANGE);
    assert(rudp_internet_checksum(odd_bytes, sizeof(odd_bytes)) == UINT16_C(0xfbfd));

    packet.sacks[0].start = packet.ack + 1U;
    packet.sacks[0].end = packet.ack + 2U;
    assert(rudp_packet_encode(&packet, encoded, sizeof(encoded), &length) == RUDP_PACKET_OK);
    encoded[RUDP_HEADER_SIZE + 4U] = 0U;
    encoded[RUDP_HEADER_SIZE + 5U] = 0U;
    encoded[RUDP_HEADER_SIZE + 6U] = 0U;
    encoded[RUDP_HEADER_SIZE + 7U] = 6U;
    checksum(encoded, length);
    assert(rudp_packet_decode(&decoded, encoded, length) == RUDP_PACKET_ERR_SACK_RANGE);
}

static void test_random_inputs(void)
{
    uint32_t state = UINT32_C(0x4d595df4);
    uint8_t bytes[1200];
    size_t iteration;

    for (iteration = 0U; iteration < 20000U; ++iteration) {
        struct rudp_packet decoded;
        size_t index;
        size_t length;

        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        length = state % sizeof(bytes);
        for (index = 0U; index < length; ++index) {
            state = state * UINT32_C(1664525) + UINT32_C(1013904223);
            bytes[index] = (uint8_t)(state >> 24U);
        }
        (void)rudp_packet_decode(&decoded, bytes, length);
    }
}

int main(void)
{
    test_golden_syn();
    test_packet_types();
    test_sacks();
    test_rejections();
    test_sack_ranges_and_checksum();
    test_random_inputs();
    puts("packet tests passed");
    return 0;
}
