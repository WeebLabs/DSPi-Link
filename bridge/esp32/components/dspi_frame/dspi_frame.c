/*
 * dspi_frame.c - builds DSPi UART requests and parses responses and
 * notifications. Implements bridge spec 7.1; see dspi_frame.h for the frame
 * layout. The uart task's link engine (dspi_link.c) is its only user on the
 * bridge; the host tests drive it directly.
 *
 * Portable: no ESP-IDF headers.
 */

#include "dspi_frame.h"

uint16_t dspi_crc16_update(uint16_t crc, const uint8_t *data, size_t length)
{
    /* CRC-16/CCITT-FALSE: polynomial 0x1021, most significant bit first, no
     * reflection and no final XOR. Bitwise rather than table-driven: frames
     * are short and the 512-byte table is not worth its flash. */
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void put_u16_le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFF);
    out[1] = (uint8_t)(value >> 8);
}

void dspi_frame_encode_request(const dspi_request_t *request,
                               uint8_t header[DSPI_FRAME_REQUEST_HEADER_BYTES],
                               uint8_t crc[DSPI_FRAME_CRC_BYTES])
{
    header[0] = DSPI_FRAME_SYNC;
    header[1] = request->is_get ? DSPI_FRAME_TYPE_GET_REQUEST : DSPI_FRAME_TYPE_SET_REQUEST;
    header[2] = request->request;
    put_u16_le(&header[3], request->value);
    put_u16_le(&header[5], request->index);
    put_u16_le(&header[7], request->length);

    /* The CRC covers everything after the sync byte, payload included. */
    uint16_t sum =
        dspi_crc16_update(DSPI_CRC_INITIAL, &header[1], DSPI_FRAME_REQUEST_HEADER_BYTES - 1);
    if (!request->is_get && request->length > 0) {
        sum = dspi_crc16_update(sum, request->payload, request->length);
    }
    put_u16_le(crc, sum);
}

void dspi_parser_init(dspi_parser_t *parser, uint8_t *buffer, size_t capacity)
{
    parser->buffer = buffer;
    parser->capacity = capacity;
    dspi_parser_reset(parser);
}

void dspi_parser_reset(dspi_parser_t *parser)
{
    parser->stage = DSPI_PARSER_WAIT_SYNC;
    parser->length = 0;
    parser->received = 0;
    parser->crc = DSPI_CRC_INITIAL;
}

bool dspi_parser_in_frame(const dspi_parser_t *parser)
{
    return parser->stage != DSPI_PARSER_WAIT_SYNC;
}

static bool is_device_frame_type(uint8_t type)
{
    return type == DSPI_FRAME_TYPE_SET_RESPONSE || type == DSPI_FRAME_TYPE_GET_RESPONSE ||
           type == DSPI_FRAME_TYPE_NOTIFICATION;
}

/* Adds one byte after the sync byte to the running CRC. */
static void add_to_crc(dspi_parser_t *parser, uint8_t byte)
{
    parser->crc = dspi_crc16_update(parser->crc, &byte, 1);
}

/* Handles the bytes before the payload: type, status and length. */
static dspi_parse_result_t feed_header(dspi_parser_t *parser, uint8_t byte)
{
    switch (parser->stage) {
    case DSPI_PARSER_WAIT_TYPE:
        if (!is_device_frame_type(byte)) {
            /* Noise. A second 0xA5 may be the real start of a frame. */
            parser->stage =
                (byte == DSPI_FRAME_SYNC) ? DSPI_PARSER_WAIT_TYPE : DSPI_PARSER_WAIT_SYNC;
            return DSPI_PARSE_NOTHING;
        }
        parser->type = byte;
        parser->stage = DSPI_PARSER_WAIT_STATUS;
        break;
    case DSPI_PARSER_WAIT_STATUS:
        parser->status = byte;
        parser->stage = DSPI_PARSER_WAIT_LENGTH_LOW;
        break;
    case DSPI_PARSER_WAIT_LENGTH_LOW:
        parser->length = byte;
        parser->stage = DSPI_PARSER_WAIT_LENGTH_HIGH;
        break;
    default: /* DSPI_PARSER_WAIT_LENGTH_HIGH */
        parser->length |= (uint16_t)(byte << 8);
        if (parser->length > parser->capacity) {
            return DSPI_PARSE_TOO_LONG;
        }
        parser->received = 0;
        parser->stage = parser->length > 0 ? DSPI_PARSER_WAIT_PAYLOAD : DSPI_PARSER_WAIT_CRC_LOW;
        break;
    }
    add_to_crc(parser, byte);
    return DSPI_PARSE_NOTHING;
}

/* Describes the frame in the parser, finished or discarded. */
static void describe_frame(const dspi_parser_t *parser, dspi_frame_t *frame)
{
    frame->type = parser->type;
    frame->status = parser->status;
    frame->length = parser->length;
    frame->payload = parser->buffer;
}

dspi_parse_result_t dspi_parser_feed(dspi_parser_t *parser, uint8_t byte, dspi_frame_t *frame)
{
    switch (parser->stage) {
    case DSPI_PARSER_WAIT_SYNC:
        if (byte == DSPI_FRAME_SYNC) {
            parser->crc = DSPI_CRC_INITIAL;
            parser->stage = DSPI_PARSER_WAIT_TYPE;
        }
        return DSPI_PARSE_NOTHING;

    case DSPI_PARSER_WAIT_TYPE:
    case DSPI_PARSER_WAIT_STATUS:
    case DSPI_PARSER_WAIT_LENGTH_LOW:
    case DSPI_PARSER_WAIT_LENGTH_HIGH:
        if (feed_header(parser, byte) == DSPI_PARSE_TOO_LONG) {
            describe_frame(parser, frame);
            frame->payload = NULL;
            dspi_parser_reset(parser);
            return DSPI_PARSE_TOO_LONG;
        }
        return DSPI_PARSE_NOTHING;

    case DSPI_PARSER_WAIT_PAYLOAD:
        parser->buffer[parser->received++] = byte;
        add_to_crc(parser, byte);
        if (parser->received == parser->length) {
            parser->stage = DSPI_PARSER_WAIT_CRC_LOW;
        }
        return DSPI_PARSE_NOTHING;

    case DSPI_PARSER_WAIT_CRC_LOW:
        parser->crc_low = byte;
        parser->stage = DSPI_PARSER_WAIT_CRC_HIGH;
        return DSPI_PARSE_NOTHING;

    case DSPI_PARSER_WAIT_CRC_HIGH: {
        const uint16_t received_crc = (uint16_t)(parser->crc_low | (byte << 8));
        parser->stage = DSPI_PARSER_WAIT_SYNC;
        describe_frame(parser, frame);
        if (received_crc != parser->crc) {
            frame->payload = NULL;
            return DSPI_PARSE_BAD_CRC;
        }
        return DSPI_PARSE_FRAME;
    }
    }

    /* Unreachable with a valid stage; recover rather than trust bad state. */
    dspi_parser_reset(parser);
    return DSPI_PARSE_NOTHING;
}
