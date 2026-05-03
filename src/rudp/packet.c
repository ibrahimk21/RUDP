#include "rudp/packet.h"

#include <stdbool.h>
#include <string.h>

enum { RUDP_METADATA_SIZE = 24, RUDP_ABORT_SIZE = 4 };

static uint16_t read_u16(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint32_t read_u32(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) | ((uint32_t)input[1] << 16U) | ((uint32_t)input[2] << 8U) |
           input[3];
}

static uint64_t read_u64(const uint8_t *input)
{
    uint64_t result = 0;
    size_t index;

    for (index = 0; index < 8U; ++index) {
        result = (result << 8U) | input[index];
    }
    return result;
}

static void write_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void write_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static void write_u64(uint8_t *output, uint64_t value)
{
    size_t index;

    for (index = 0; index < 8U; ++index) {
        output[7U - index] = (uint8_t)value;
        value >>= 8U;
    }
}

static bool valid_type(uint8_t type)
{
    return type >= RUDP_PACKET_SYN && type <= RUDP_PACKET_ABORT;
}

static bool serial_after(uint32_t left, uint32_t right)
{
    const uint32_t distance = left - right;
    return distance != 0U && distance < UINT32_C(0x80000000);
}

static bool fields_are_zero(const struct rudp_packet *packet)
{
    return packet->seq == 0U && packet->ack == 0U && packet->receive_limit == 0U;
}

static enum rudp_packet_error validate_sacks(const struct rudp_packet *packet)
{
    uint8_t index;
    uint32_t previous_end = packet->ack;

    if (packet->type != RUDP_PACKET_ACK && packet->sack_count != 0U) {
        return RUDP_PACKET_ERR_SACK_COUNT;
    }
    if (packet->sack_count > RUDP_MAX_SACK_BLOCKS) {
        return RUDP_PACKET_ERR_SACK_COUNT;
    }

    for (index = 0; index < packet->sack_count; ++index) {
        const struct rudp_sack_block *block = &packet->sacks[index];
        if (!serial_after(block->end, block->start) || !serial_after(block->start, packet->ack) ||
            (index != 0U && !serial_after(block->start, previous_end))) {
            return RUDP_PACKET_ERR_SACK_RANGE;
        }
        previous_end = block->end;
    }
    return RUDP_PACKET_OK;
}

static enum rudp_packet_error validate_packet(const struct rudp_packet *packet,
                                              size_t *payload_length)
{
    enum rudp_packet_error error;

    if (!valid_type((uint8_t)packet->type)) {
        return RUDP_PACKET_ERR_TYPE;
    }
    error = validate_sacks(packet);
    if (error != RUDP_PACKET_OK) {
        return error;
    }

    switch (packet->type) {
    case RUDP_PACKET_SYN:
        if (packet->server_nonce != 0U || !fields_are_zero(packet)) {
            return RUDP_PACKET_ERR_FIELD;
        }
        *payload_length = RUDP_METADATA_SIZE;
        break;
    case RUDP_PACKET_SYN_ACK:
        if (!fields_are_zero(packet)) {
            return RUDP_PACKET_ERR_FIELD;
        }
        *payload_length = RUDP_METADATA_SIZE;
        break;
    case RUDP_PACKET_OPEN:
    case RUDP_PACKET_PROBE:
        if (!fields_are_zero(packet)) {
            return RUDP_PACKET_ERR_FIELD;
        }
        *payload_length = 0U;
        break;
    case RUDP_PACKET_OPEN_ACK:
        if (packet->seq != 0U) {
            return RUDP_PACKET_ERR_FIELD;
        }
        *payload_length = 0U;
        break;
    case RUDP_PACKET_DATA:
        if (packet->ack != 0U || packet->receive_limit != 0U || packet->data == NULL ||
            packet->data_length == 0U || packet->data_length > RUDP_MAX_DATA_PAYLOAD) {
            return RUDP_PACKET_ERR_FIELD;
        }
        *payload_length = packet->data_length;
        break;
    case RUDP_PACKET_ACK:
        if (packet->seq != 0U) {
            return RUDP_PACKET_ERR_FIELD;
        }
        *payload_length = 0U;
        break;
    case RUDP_PACKET_FIN:
        if (packet->ack != 0U || packet->receive_limit != 0U) {
            return RUDP_PACKET_ERR_FIELD;
        }
        *payload_length = RUDP_METADATA_SIZE;
        break;
    case RUDP_PACKET_FIN_ACK:
        *payload_length = RUDP_METADATA_SIZE;
        break;
    case RUDP_PACKET_ABORT:
        if (!fields_are_zero(packet)) {
            return RUDP_PACKET_ERR_FIELD;
        }
        *payload_length = RUDP_ABORT_SIZE;
        break;
    default:
        return RUDP_PACKET_ERR_TYPE;
    }
    return RUDP_PACKET_OK;
}

uint16_t rudp_internet_checksum(const uint8_t *bytes, size_t length)
{
    uint32_t sum = 0U;
    size_t index;

    for (index = 0; index + 1U < length; index += 2U) {
        sum += ((uint32_t)bytes[index] << 8U) | bytes[index + 1U];
        sum = (sum & UINT32_C(0xffff)) + (sum >> 16U);
    }
    if (index < length) {
        sum += (uint32_t)bytes[index] << 8U;
        sum = (sum & UINT32_C(0xffff)) + (sum >> 16U);
    }
    while ((sum >> 16U) != 0U) {
        sum = (sum & UINT32_C(0xffff)) + (sum >> 16U);
    }
    return (uint16_t)~sum;
}

enum rudp_packet_error rudp_packet_encode(const struct rudp_packet *packet, uint8_t *output,
                                          size_t output_capacity, size_t *output_length)
{
    enum rudp_packet_error error;
    size_t payload_length;
    size_t total_length;
    size_t offset;
    uint8_t index;

    if (packet == NULL || output == NULL || output_length == NULL) {
        return RUDP_PACKET_ERR_ARGUMENT;
    }
    error = validate_packet(packet, &payload_length);
    if (error != RUDP_PACKET_OK) {
        return error;
    }
    total_length = RUDP_HEADER_SIZE + ((size_t)packet->sack_count * 8U) + payload_length;
    if (total_length > output_capacity) {
        return RUDP_PACKET_ERR_CAPACITY;
    }

    output[0] = RUDP_VERSION;
    output[1] = (uint8_t)packet->type;
    output[2] = packet->sack_count;
    output[3] = 0U;
    write_u64(output + 4U, packet->client_nonce);
    write_u64(output + 12U, packet->server_nonce);
    write_u32(output + 20U, packet->seq);
    write_u32(output + 24U, packet->ack);
    write_u32(output + 28U, packet->receive_limit);
    write_u16(output + 32U, (uint16_t)payload_length);
    write_u16(output + 34U, 0U);

    offset = RUDP_HEADER_SIZE;
    for (index = 0; index < packet->sack_count; ++index) {
        write_u32(output + offset, packet->sacks[index].start);
        write_u32(output + offset + 4U, packet->sacks[index].end);
        offset += 8U;
    }
    if (packet->type == RUDP_PACKET_DATA) {
        memcpy(output + offset, packet->data, payload_length);
    } else if (payload_length == RUDP_METADATA_SIZE) {
        write_u64(output + offset, packet->transfer_length);
        memcpy(output + offset + 8U, packet->digest, sizeof(packet->digest));
    } else if (packet->type == RUDP_PACKET_ABORT) {
        write_u32(output + offset, packet->abort_code);
    }
    write_u16(output + 34U, rudp_internet_checksum(output, total_length));
    *output_length = total_length;
    return RUDP_PACKET_OK;
}

enum rudp_packet_error rudp_packet_decode(struct rudp_packet *packet, const uint8_t *input,
                                          size_t input_length)
{
    enum rudp_packet_error error;
    size_t payload_length;
    size_t canonical_payload_length;
    size_t expected_length;
    size_t offset;
    uint8_t index;

    if (packet == NULL || input == NULL) {
        return RUDP_PACKET_ERR_ARGUMENT;
    }
    if (input_length < RUDP_HEADER_SIZE) {
        return RUDP_PACKET_ERR_TRUNCATED;
    }
    if (input[0] != RUDP_VERSION) {
        return RUDP_PACKET_ERR_VERSION;
    }
    if (!valid_type(input[1])) {
        return RUDP_PACKET_ERR_TYPE;
    }
    if (input[3] != 0U) {
        return RUDP_PACKET_ERR_RESERVED;
    }
    if (input[2] > RUDP_MAX_SACK_BLOCKS) {
        return RUDP_PACKET_ERR_SACK_COUNT;
    }

    payload_length = read_u16(input + 32U);
    expected_length = RUDP_HEADER_SIZE + ((size_t)input[2] * 8U) + payload_length;
    if (expected_length != input_length) {
        return RUDP_PACKET_ERR_LENGTH;
    }
    if (rudp_internet_checksum(input, input_length) != 0U) {
        return RUDP_PACKET_ERR_CHECKSUM;
    }

    memset(packet, 0, sizeof(*packet));
    packet->type = (enum rudp_packet_type)input[1];
    packet->client_nonce = read_u64(input + 4U);
    packet->server_nonce = read_u64(input + 12U);
    packet->seq = read_u32(input + 20U);
    packet->ack = read_u32(input + 24U);
    packet->receive_limit = read_u32(input + 28U);
    packet->sack_count = input[2];
    offset = RUDP_HEADER_SIZE;
    for (index = 0; index < packet->sack_count; ++index) {
        packet->sacks[index].start = read_u32(input + offset);
        packet->sacks[index].end = read_u32(input + offset + 4U);
        offset += 8U;
    }
    if (packet->type == RUDP_PACKET_DATA) {
        packet->data = input + offset;
        packet->data_length = (uint16_t)payload_length;
    } else if (payload_length == RUDP_METADATA_SIZE) {
        packet->transfer_length = read_u64(input + offset);
        memcpy(packet->digest, input + offset + 8U, sizeof(packet->digest));
    } else if (packet->type == RUDP_PACKET_ABORT) {
        packet->abort_code = read_u32(input + offset);
    }

    error = validate_packet(packet, &canonical_payload_length);
    if (error != RUDP_PACKET_OK) {
        return error;
    }
    if (canonical_payload_length != payload_length) {
        return RUDP_PACKET_ERR_LENGTH;
    }
    return RUDP_PACKET_OK;
}

const char *rudp_packet_error_string(enum rudp_packet_error error)
{
    static const char *const messages[] = {
        "ok",
        "invalid argument",
        "output buffer too small",
        "truncated",
        "unsupported version",
        "invalid type",
        "reserved field set",
        "invalid SACK count",
        "invalid length",
        "checksum mismatch",
        "invalid packet field",
        "invalid SACK range",
    };

    if ((unsigned int)error >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown packet error";
    }
    return messages[error];
}
