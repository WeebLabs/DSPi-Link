/*
 * dspi_frame.h - the DSPi's UART frames: building requests and parsing
 * responses and notifications.
 *
 * Implements bridge spec 7.1, which follows the firmware's
 * control_interfaces_spec.md section 3. Every frame starts with the sync
 * byte 0xA5 and ends with a CRC-16/CCITT-FALSE over every byte after the
 * sync byte, sent low byte first.
 *
 *   request:      A5 type bReq wValue(2) wIndex(2) wLength(2) [payload] crc(2)
 *   response:     A5 type status length(2) [payload] crc(2)
 *   notification: A5 40   00     length(2) packet    crc(2)
 *
 * Portable: no ESP-IDF headers. The parser never allocates; the caller
 * gives it the payload buffer.
 */

#ifndef DSPI_FRAME_H
#define DSPI_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DSPI_FRAME_SYNC 0xA5

/* Frame type bytes. */
#define DSPI_FRAME_TYPE_SET_REQUEST 0x01
#define DSPI_FRAME_TYPE_GET_REQUEST 0x02
#define DSPI_FRAME_TYPE_NOTIFICATION 0x40
#define DSPI_FRAME_TYPE_SET_RESPONSE 0x81
#define DSPI_FRAME_TYPE_GET_RESPONSE 0x82

/* A request's bytes before its payload: sync, type, bRequest and the three
 * 16-bit fields. */
#define DSPI_FRAME_REQUEST_HEADER_BYTES 9

/* Bytes a response or notification adds around its payload: sync, type,
 * status, length (2) and CRC (2). Used to work out wire times. */
#define DSPI_FRAME_RESPONSE_OVERHEAD_BYTES 7

#define DSPI_FRAME_CRC_BYTES 2

/* Firmware status codes, carried unchanged into Link CMD responses (Link
 * spec 8.1). */
#define DSPI_STATUS_OK 0x00
#define DSPI_STATUS_BUSY 0x01
#define DSPI_STATUS_ERROR 0x02
#define DSPI_STATUS_BLOCKED 0x03
#define DSPI_STATUS_BULK_LOCKED 0x04
#define DSPI_STATUS_CRC_ERROR 0x05
#define DSPI_STATUS_OVERSIZE 0x06
#define DSPI_STATUS_FRAME_ERROR 0x07

/* ---------------------------------------------------------------------------
 * CRC
 * ------------------------------------------------------------------------- */

/* The CRC's starting value. */
#define DSPI_CRC_INITIAL 0xFFFF

/* Folds `length` bytes into `crc` and returns the result, so a CRC can be
 * built up over several pieces. Start from DSPI_CRC_INITIAL. */
uint16_t dspi_crc16_update(uint16_t crc, const uint8_t *data, size_t length);

/* ---------------------------------------------------------------------------
 * Requests
 * ------------------------------------------------------------------------- */

/* One vendor request, shaped as over USB. */
typedef struct {
    bool is_get;            /* GET (device to host) or SET (host to device) */
    uint8_t request;        /* bRequest */
    uint16_t value;         /* wValue */
    uint16_t index;         /* wIndex */
    uint16_t length;        /* SET: payload bytes. GET: most bytes wanted, 0 for all */
    const uint8_t *payload; /* SET only: `length` bytes; NULL for a GET */
} dspi_request_t;

/*
 * Builds the request's header and CRC without copying its payload, so a
 * 6 KB bulk SET goes out as header, payload and CRC in three writes.
 *
 * header: receives DSPI_FRAME_REQUEST_HEADER_BYTES bytes.
 * crc:    receives the 2 CRC bytes to send after the payload.
 */
void dspi_frame_encode_request(const dspi_request_t *request,
                               uint8_t header[DSPI_FRAME_REQUEST_HEADER_BYTES],
                               uint8_t crc[DSPI_FRAME_CRC_BYTES]);

/* ---------------------------------------------------------------------------
 * Parsing responses and notifications
 * ------------------------------------------------------------------------- */

/* What one byte fed to the parser completed, if anything. */
typedef enum {
    DSPI_PARSE_NOTHING,  /* No frame finished with this byte */
    DSPI_PARSE_FRAME,    /* A whole, valid frame is in the parser */
    DSPI_PARSE_BAD_CRC,  /* A frame finished but its CRC was wrong; discarded */
    DSPI_PARSE_TOO_LONG, /* A frame announced more payload than the buffer holds; discarded */
} dspi_parse_result_t;

/* A finished frame. `payload` points into the parser's buffer and stays
 * valid until the next byte is fed. */
typedef struct {
    uint8_t type;   /* DSPI_FRAME_TYPE_SET_RESPONSE, _GET_RESPONSE or _NOTIFICATION */
    uint8_t status; /* DSPI_STATUS_*; 0 for a notification */
    uint16_t length;
    const uint8_t *payload;
} dspi_frame_t;

/* Which part of a frame the parser expects next. */
typedef enum {
    DSPI_PARSER_WAIT_SYNC,
    DSPI_PARSER_WAIT_TYPE,
    DSPI_PARSER_WAIT_STATUS,
    DSPI_PARSER_WAIT_LENGTH_LOW,
    DSPI_PARSER_WAIT_LENGTH_HIGH,
    DSPI_PARSER_WAIT_PAYLOAD,
    DSPI_PARSER_WAIT_CRC_LOW,
    DSPI_PARSER_WAIT_CRC_HIGH,
} dspi_parser_stage_t;

/* Parser state. Treat as opaque; use the functions below. */
typedef struct {
    dspi_parser_stage_t stage;
    uint8_t type;
    uint8_t status;
    uint16_t length;
    uint16_t received;
    uint16_t crc;
    uint8_t crc_low;
    uint8_t *buffer;
    size_t capacity;
} dspi_parser_t;

/* Prepares a parser that stores payloads in `buffer` (capacity bytes). */
void dspi_parser_init(dspi_parser_t *parser, uint8_t *buffer, size_t capacity);

/* Drops any partly received frame and waits for the next sync byte. */
void dspi_parser_reset(dspi_parser_t *parser);

/* True while the parser is part way through a frame. */
bool dspi_parser_in_frame(const dspi_parser_t *parser);

/*
 * Feeds one received byte.
 *
 * Unknown type bytes and bytes outside a frame are noise and are skipped,
 * as the firmware does. On DSPI_PARSE_FRAME, `frame` describes the frame.
 */
dspi_parse_result_t dspi_parser_feed(dspi_parser_t *parser, uint8_t byte, dspi_frame_t *frame);

#endif /* DSPI_FRAME_H */
