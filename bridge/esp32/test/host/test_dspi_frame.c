/*
 * test_dspi_frame.c - the DSPi UART frame encoder, parser and CRC (bridge
 * spec 7.1), against the firmware spec's reference values.
 */

#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dspi_frame.h"
#include "test_frames.h"

static void crc_matches_the_reference_check_value(void)
{
    /* control_interfaces_spec.md 3.3: "123456789" gives 0x29B1. */
    const uint8_t text[] = "123456789";
    CHECK(dspi_crc16_update(DSPI_CRC_INITIAL, text, 9) == 0x29B1);

    /* Folding in pieces gives the same result as one pass. */
    const uint16_t first = dspi_crc16_update(DSPI_CRC_INITIAL, text, 4);
    CHECK(dspi_crc16_update(first, text + 4, 5) == 0x29B1);
}

static void set_request_matches_the_firmware_example(void)
{
    /* control_interfaces_spec.md 6.1: master volume to -20 dB. */
    const uint8_t volume[] = {0x00, 0x00, 0xA0, 0xC1};
    const dspi_request_t request = {
        .is_get = false,
        .request = 0xD2,
        .value = 0,
        .index = 0,
        .length = 4,
        .payload = volume,
    };
    uint8_t header[DSPI_FRAME_REQUEST_HEADER_BYTES];
    uint8_t crc[DSPI_FRAME_CRC_BYTES];
    dspi_frame_encode_request(&request, header, crc);

    const uint8_t expected_header[] = {0xA5, 0x01, 0xD2, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00};
    CHECK(memcmp(header, expected_header, sizeof(expected_header)) == 0);
    CHECK(crc[0] == 0xCF && crc[1] == 0x6D);
}

static void get_request_has_no_payload_in_its_crc(void)
{
    const dspi_request_t request = {
        .is_get = true,
        .request = 0x7F,
        .value = 0x1234,
        .index = 2,
        .length = 7,
    };
    uint8_t header[DSPI_FRAME_REQUEST_HEADER_BYTES];
    uint8_t crc[DSPI_FRAME_CRC_BYTES];
    dspi_frame_encode_request(&request, header, crc);

    const uint8_t expected_header[] = {0xA5, 0x02, 0x7F, 0x34, 0x12, 0x02, 0x00, 0x07, 0x00};
    CHECK(memcmp(header, expected_header, sizeof(expected_header)) == 0);
    const uint16_t sum = dspi_crc16_update(DSPI_CRC_INITIAL, header + 1, 8);
    CHECK(crc[0] == (sum & 0xFF) && crc[1] == (sum >> 8));
}

/* Notes one result: the last that was not NOTHING, and a count of frames. */
static void note_result(dspi_parse_result_t result, dspi_parse_result_t *last, int *frames_seen)
{
    if (result != DSPI_PARSE_NOTHING) {
        *last = result;
    }
    if (result == DSPI_PARSE_FRAME && frames_seen != NULL) {
        (*frames_seen)++;
    }
}

/* Rescans whatever the parser kept, as the link does before each new byte. */
static void rescan_all(dspi_parser_t *parser, dspi_frame_t *frame, dspi_parse_result_t *last,
                       int *frames_seen)
{
    dspi_parse_result_t result;
    while (dspi_parser_rescan(parser, &result, frame)) {
        note_result(result, last, frames_seen);
    }
}

/* Feeds `bytes`, rescanning after each as the link does, and returns the
 * last non-NOTHING result, copying the frame. */
static dspi_parse_result_t feed_all(dspi_parser_t *parser, const uint8_t *bytes, size_t count,
                                    dspi_frame_t *frame, int *frames_seen)
{
    dspi_parse_result_t last = DSPI_PARSE_NOTHING;
    for (size_t i = 0; i < count; i++) {
        note_result(dspi_parser_feed(parser, bytes[i], frame), &last, frames_seen);
        rescan_all(parser, frame, &last, frames_seen);
    }
    return last;
}

static void parses_a_get_response(void)
{
    const uint8_t platform[] = {0x01, 0x01, 0x17, 0x09, 0x01, 0x07, 0x00};
    uint8_t wire[64];
    const size_t length = test_build_device_frame(DSPI_FRAME_TYPE_GET_RESPONSE, DSPI_STATUS_OK,
                                                  platform, sizeof(platform), wire);
    uint8_t buffer[32];
    dspi_parser_t parser;
    dspi_parser_init(&parser, buffer, sizeof(buffer));
    dspi_frame_t frame;

    CHECK(feed_all(&parser, wire, length, &frame, NULL) == DSPI_PARSE_FRAME);
    CHECK(frame.type == DSPI_FRAME_TYPE_GET_RESPONSE);
    CHECK(frame.status == DSPI_STATUS_OK);
    CHECK(frame.length == sizeof(platform));
    CHECK(memcmp(frame.payload, platform, sizeof(platform)) == 0);
    CHECK(!dspi_parser_in_frame(&parser));
}

static void parses_an_empty_set_response_and_a_notification(void)
{
    uint8_t wire[64];
    size_t length =
        test_build_device_frame(DSPI_FRAME_TYPE_SET_RESPONSE, DSPI_STATUS_BUSY, NULL, 0, wire);
    const uint8_t packet[] = {0x02, 0x01, 0x05, 0x00};
    length += test_build_device_frame(DSPI_FRAME_TYPE_NOTIFICATION, 0, packet, sizeof(packet),
                                      wire + length);

    uint8_t buffer[32];
    dspi_parser_t parser;
    dspi_parser_init(&parser, buffer, sizeof(buffer));
    dspi_frame_t frame;
    int frames = 0;

    CHECK(feed_all(&parser, wire, length, &frame, &frames) == DSPI_PARSE_FRAME);
    CHECK(frames == 2);
    CHECK(frame.type == DSPI_FRAME_TYPE_NOTIFICATION);
    CHECK(frame.length == sizeof(packet));
}

static void skips_noise_and_stray_sync_bytes(void)
{
    uint8_t wire[64] = {0x00, 0xFF, 0xA5, 0x13, 0xA5}; /* Noise, a bad type, then a real sync */
    const uint8_t serial[] = {'E', '4', '6', '0'};
    /* The real frame's own sync byte follows the stray 0xA5 directly. */
    const size_t length = 5 + test_build_device_frame(DSPI_FRAME_TYPE_GET_RESPONSE, DSPI_STATUS_OK,
                                                      serial, sizeof(serial), wire + 5);
    uint8_t buffer[32];
    dspi_parser_t parser;
    dspi_parser_init(&parser, buffer, sizeof(buffer));
    dspi_frame_t frame;
    int frames = 0;

    CHECK(feed_all(&parser, wire, length, &frame, &frames) == DSPI_PARSE_FRAME);
    CHECK(frames == 1);
    CHECK(frame.length == sizeof(serial));
}

static void rejects_a_bad_crc_then_recovers(void)
{
    const uint8_t data[] = {0x11, 0x22};
    uint8_t wire[64];
    size_t length = test_build_device_frame(DSPI_FRAME_TYPE_GET_RESPONSE, DSPI_STATUS_OK, data,
                                            sizeof(data), wire);
    wire[length - 1] ^= 0x01; /* Damage the CRC */
    const size_t second = test_build_device_frame(DSPI_FRAME_TYPE_GET_RESPONSE, DSPI_STATUS_OK,
                                                  data, sizeof(data), wire + length);

    uint8_t buffer[32];
    dspi_parser_t parser;
    dspi_parser_init(&parser, buffer, sizeof(buffer));
    dspi_frame_t frame;

    CHECK(feed_all(&parser, wire, length, &frame, NULL) == DSPI_PARSE_BAD_CRC);
    /* What the damaged frame claimed, so the link can tell an answer from a
     * notification. */
    CHECK(frame.type == DSPI_FRAME_TYPE_GET_RESPONSE);
    CHECK(frame.length == sizeof(data));
    CHECK(frame.payload == NULL);
    CHECK(feed_all(&parser, wire + length, second, &frame, NULL) == DSPI_PARSE_FRAME);
}

static void rejects_a_frame_longer_than_the_buffer(void)
{
    const uint8_t data[8] = {0};
    uint8_t wire[64];
    test_build_device_frame(DSPI_FRAME_TYPE_GET_RESPONSE, DSPI_STATUS_OK, data, sizeof(data), wire);
    uint8_t buffer[DSPI_PARSER_BUFFER_BYTES(4)];
    dspi_parser_t parser;
    dspi_parser_init(&parser, buffer, sizeof(buffer));
    dspi_frame_t frame;

    /* The length bytes (offsets 3 and 4) already tell the parser it is too long. */
    CHECK(feed_all(&parser, wire, 5, &frame, NULL) == DSPI_PARSE_TOO_LONG);
    CHECK(!dspi_parser_in_frame(&parser));
    CHECK(frame.type == DSPI_FRAME_TYPE_GET_RESPONSE);
    CHECK(frame.length == sizeof(data));
}

static void reset_drops_a_partial_frame(void)
{
    const uint8_t data[] = {0x42};
    uint8_t wire[64];
    const size_t length = test_build_device_frame(DSPI_FRAME_TYPE_GET_RESPONSE, DSPI_STATUS_OK,
                                                  data, sizeof(data), wire);
    uint8_t buffer[DSPI_PARSER_BUFFER_BYTES(1)];
    dspi_parser_t parser;
    dspi_parser_init(&parser, buffer, sizeof(buffer));
    dspi_frame_t frame;

    feed_all(&parser, wire, 4, &frame, NULL);
    CHECK(dspi_parser_in_frame(&parser));
    dspi_parser_reset(&parser);
    CHECK(!dspi_parser_in_frame(&parser));
    CHECK(feed_all(&parser, wire, length, &frame, NULL) == DSPI_PARSE_FRAME);
}

/* Noise that looks like the start of a long frame, with a real frame
 * straight after: the false frame fails its CRC once it has its length,
 * and the real frame inside it is found by the rescan (audit UART-1). */
static void a_frame_inside_a_false_start_is_found(void)
{
    uint8_t wire[128] = {0xA5, DSPI_FRAME_TYPE_NOTIFICATION, 0x00, 0x20, 0x00}; /* Claims 32 */
    size_t length = 5;
    const uint8_t data[] = {0x11, 0xA5, 0x82, 0x33}; /* Sync and type inside a payload */
    length += test_build_device_frame(DSPI_FRAME_TYPE_GET_RESPONSE, DSPI_STATUS_OK, data,
                                      sizeof(data), &wire[length]);
    memset(&wire[length], 0x00, 40); /* Enough to finish the false frame */
    length += 40;

    uint8_t buffer[DSPI_PARSER_BUFFER_BYTES(64)];
    dspi_parser_t parser;
    dspi_parser_init(&parser, buffer, sizeof(buffer));
    dspi_frame_t frame;
    dspi_frame_t found = {0};
    int frames = 0;
    for (size_t i = 0; i < length; i++) {
        dspi_parse_result_t last = DSPI_PARSE_NOTHING;
        note_result(dspi_parser_feed(&parser, wire[i], &frame), &last, &frames);
        if (last == DSPI_PARSE_FRAME) {
            found = frame;
        }
        dspi_parse_result_t result;
        while (dspi_parser_rescan(&parser, &result, &frame)) {
            if (result == DSPI_PARSE_FRAME) {
                frames++;
                CHECK(frame.length == sizeof(data));
                CHECK(memcmp(frame.payload, data, sizeof(data)) == 0);
                found = frame;
            }
        }
    }
    CHECK(frames == 1);
    CHECK(found.type == DSPI_FRAME_TYPE_GET_RESPONSE);
}

/* A false start that never gets its length (the line went quiet) is
 * abandoned, and the real frame it swallowed is found. */
static void an_abandoned_frame_is_searched_again(void)
{
    uint8_t wire[64] = {0xA5, DSPI_FRAME_TYPE_SET_RESPONSE, 0x00, 0x40, 0x00}; /* Claims 64 */
    size_t length = 5;
    length +=
        test_build_device_frame(DSPI_FRAME_TYPE_SET_RESPONSE, DSPI_STATUS_OK, NULL, 0, &wire[5]);

    uint8_t buffer[DSPI_PARSER_BUFFER_BYTES(64)];
    dspi_parser_t parser;
    dspi_parser_init(&parser, buffer, sizeof(buffer));
    dspi_frame_t frame;
    int frames = 0;

    CHECK(feed_all(&parser, wire, length, &frame, &frames) == DSPI_PARSE_NOTHING);
    CHECK(frames == 0);
    CHECK(dspi_parser_in_frame(&parser));

    dspi_parser_abandon(&parser);
    CHECK(dspi_parser_rescan_pending(&parser));
    dspi_parse_result_t last = DSPI_PARSE_NOTHING;
    rescan_all(&parser, &frame, &last, &frames);
    CHECK(last == DSPI_PARSE_FRAME);
    CHECK(frames == 1);
    CHECK(frame.type == DSPI_FRAME_TYPE_SET_RESPONSE && frame.status == DSPI_STATUS_OK);

    /* Outside a frame there is nothing to abandon. */
    dspi_parser_abandon(&parser);
    CHECK(!dspi_parser_rescan_pending(&parser));
}

/* A reset drops bytes waiting to be rescanned along with the frame. */
static void reset_drops_bytes_waiting_to_be_rescanned(void)
{
    const uint8_t wire[] = {0xA5, DSPI_FRAME_TYPE_GET_RESPONSE, 0x00, 0x01, 0x00, 0xA5, 0x00, 0x00};
    uint8_t buffer[DSPI_PARSER_BUFFER_BYTES(8)];
    dspi_parser_t parser;
    dspi_parser_init(&parser, buffer, sizeof(buffer));
    dspi_frame_t frame;
    for (size_t i = 0; i < sizeof(wire); i++) {
        (void)dspi_parser_feed(&parser, wire[i], &frame);
    }
    CHECK(dspi_parser_rescan_pending(&parser));
    dspi_parser_reset(&parser);
    CHECK(!dspi_parser_rescan_pending(&parser));
    CHECK(!dspi_parser_in_frame(&parser));
}

static uint32_t random_state = 12345;

static uint8_t random_byte(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return (uint8_t)random_state;
}

/* Real frames scattered through noise rich in sync and type bytes, false
 * lengths included, are all found, once each and in order, whatever the
 * noise does to the parser in between. */
static void every_frame_in_noise_is_found_in_order(void)
{
    enum { FRAMES = 300, MAX_PAYLOAD = 48 };
    static uint8_t wire[FRAMES * (MAX_PAYLOAD + 7 + 24)];
    size_t length = 0;
    for (int n = 0; n < FRAMES; n++) {
        const size_t noise = random_byte() % 24;
        for (size_t i = 0; i < noise; i++) {
            const uint8_t pick = random_byte() % 6;
            wire[length++] = pick == 0   ? 0xA5
                             : pick == 1 ? DSPI_FRAME_TYPE_NOTIFICATION
                             : pick == 2 ? DSPI_FRAME_TYPE_GET_RESPONSE
                                         : random_byte();
        }
        uint8_t payload[MAX_PAYLOAD];
        const size_t size = 2 + random_byte() % (MAX_PAYLOAD - 2);
        for (size_t i = 0; i < size; i++) {
            payload[i] = random_byte() % 4 == 0 ? 0xA5 : random_byte();
        }
        payload[0] = (uint8_t)n; /* Numbers the frame */
        payload[1] = (uint8_t)(n >> 8);
        length +=
            test_build_device_frame(DSPI_FRAME_TYPE_NOTIFICATION, 0, payload, size, &wire[length]);
    }

    uint8_t buffer[DSPI_PARSER_BUFFER_BYTES(MAX_PAYLOAD + 32)];
    dspi_parser_t parser;
    dspi_parser_init(&parser, buffer, sizeof(buffer));
    int expected = 0;
    bool in_order = true;
    for (size_t i = 0; i <= length; i++) {
        dspi_frame_t frame;
        dspi_parse_result_t result = DSPI_PARSE_NOTHING;
        if (i < length) {
            result = dspi_parser_feed(&parser, wire[i], &frame);
        } else {
            dspi_parser_abandon(&parser); /* The line goes quiet */
        }
        for (;;) {
            if (result == DSPI_PARSE_FRAME) {
                const int number = frame.payload[0] | (frame.payload[1] << 8);
                in_order = in_order && number == expected;
                expected++;
            }
            if (!dspi_parser_rescan(&parser, &result, &frame)) {
                break;
            }
        }
    }
    CHECK(in_order);
    CHECK(expected == FRAMES);
}

void test_dspi_frame(void)
{
    RUN(crc_matches_the_reference_check_value);
    RUN(set_request_matches_the_firmware_example);
    RUN(get_request_has_no_payload_in_its_crc);
    RUN(parses_a_get_response);
    RUN(parses_an_empty_set_response_and_a_notification);
    RUN(skips_noise_and_stray_sync_bytes);
    RUN(rejects_a_bad_crc_then_recovers);
    RUN(rejects_a_frame_longer_than_the_buffer);
    RUN(reset_drops_a_partial_frame);
    RUN(a_frame_inside_a_false_start_is_found);
    RUN(an_abandoned_frame_is_searched_again);
    RUN(reset_drops_bytes_waiting_to_be_rescanned);
    RUN(every_frame_in_noise_is_found_in_order);
}
