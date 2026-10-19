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

/* Feeds `bytes` and returns the last non-NOTHING result, copying the frame. */
static dspi_parse_result_t feed_all(dspi_parser_t *parser, const uint8_t *bytes, size_t count,
                                    dspi_frame_t *frame, int *frames_seen)
{
    dspi_parse_result_t last = DSPI_PARSE_NOTHING;
    for (size_t i = 0; i < count; i++) {
        const dspi_parse_result_t result = dspi_parser_feed(parser, bytes[i], frame);
        if (result != DSPI_PARSE_NOTHING) {
            last = result;
        }
        if (result == DSPI_PARSE_FRAME && frames_seen != NULL) {
            (*frames_seen)++;
        }
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
    uint8_t buffer[4];
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
    uint8_t buffer[8];
    dspi_parser_t parser;
    dspi_parser_init(&parser, buffer, sizeof(buffer));
    dspi_frame_t frame;

    feed_all(&parser, wire, 4, &frame, NULL);
    CHECK(dspi_parser_in_frame(&parser));
    dspi_parser_reset(&parser);
    CHECK(!dspi_parser_in_frame(&parser));
    CHECK(feed_all(&parser, wire, length, &frame, NULL) == DSPI_PARSE_FRAME);
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
}
