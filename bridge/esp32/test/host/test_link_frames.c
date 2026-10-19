/*
 * test_link_frames.c - the Link binary frames (Link spec 8), byte for byte
 * against fixtures/frames.
 */

#include <stdint.h>
#include <string.h>

#include "check.h"
#include "link_frames.h"
#include "test_fixture.h"

static uint8_t fixture[256];
static uint8_t encoded[256];

static void decodes_the_set_master_volume_request(void)
{
    const size_t length =
        test_fixture_read("frames/cmd_request_set_master_volume.bin", fixture, sizeof(fixture));
    link_cmd_request_t request;
    CHECK(link_decode_cmd_request(fixture, length, &request));
    CHECK(request.tag == 7);
    CHECK(request.handle == 0);
    CHECK(!request.is_get);
    CHECK(request.request == 0xD2);
    CHECK(request.value == 0 && request.index == 2 && request.length == 4);
    const uint8_t volume[] = {0x00, 0x00, 0xA0, 0xC1};
    CHECK(memcmp(request.payload, volume, 4) == 0);
}

static void decodes_the_get_platform_request(void)
{
    const size_t length =
        test_fixture_read("frames/cmd_request_get_platform.bin", fixture, sizeof(fixture));
    link_cmd_request_t request;
    CHECK(link_decode_cmd_request(fixture, length, &request));
    CHECK(request.tag == 8 && request.handle == 1 && request.is_get);
    CHECK(request.request == 0x7F && request.length == 7);
    CHECK(request.payload == NULL);
}

static void rejects_malformed_requests(void)
{
    const size_t length =
        test_fixture_read("frames/cmd_request_set_master_volume.bin", fixture, sizeof(fixture));
    link_cmd_request_t request;
    CHECK(!link_decode_cmd_request(fixture, length - 1, &request)); /* Payload cut short */
    fixture[5] = 2;                                                 /* Not SET or GET */
    CHECK(!link_decode_cmd_request(fixture, length, &request));
    CHECK(!link_decode_cmd_request(fixture, 3, &request));
}

static void encodes_cmd_responses(void)
{
    size_t length = test_fixture_read("frames/cmd_response_platform.bin", fixture, sizeof(fixture));
    const uint8_t platform[] = {0x01, 0x01, 0x17, 0x09, 0x01, 0x07, 0x00};
    size_t made = link_encode_cmd_response(encoded, sizeof(encoded), 8, 0x00, platform, 7);
    CHECK(made == length && memcmp(encoded, fixture, length) == 0);

    length = test_fixture_read("frames/cmd_response_denied.bin", fixture, sizeof(fixture));
    made = link_encode_cmd_response(encoded, sizeof(encoded), 9, LINK_STATUS_DENIED, NULL, 0);
    CHECK(made == length && memcmp(encoded, fixture, length) == 0);
}

static void encodes_a_notify(void)
{
    const size_t length =
        test_fixture_read("frames/notify_param_changed.bin", fixture, sizeof(fixture));
    /* The packet is everything after the 8-byte NOTIFY header. */
    const size_t made =
        link_encode_notify(encoded, sizeof(encoded), 3, 0, 12, fixture + 8, length - 8);
    CHECK(made == length && memcmp(encoded, fixture, length) == 0);
}

static void encodes_an_empty_notify_without_a_packet(void)
{
    /* No packet at all: the encoder must not touch the NULL pointer. */
    const size_t made = link_encode_notify(encoded, sizeof(encoded), 1, 0, 0, NULL, 0);
    CHECK(made == LINK_NOTIFY_HEADER_BYTES);
    CHECK(encoded[0] == LINK_FRAME_NOTIFY);
}

static void encodes_a_poll(void)
{
    const size_t length = test_fixture_read("frames/poll_status.bin", fixture, sizeof(fixture));
    uint8_t status[41];
    for (int i = 0; i < 41; i++) {
        status[i] = (uint8_t)i;
    }
    const size_t made = link_encode_poll(encoded, sizeof(encoded), 100, 0, 2, status, 41);
    CHECK(made == length && memcmp(encoded, fixture, length) == 0);
}

static void handles_fwdata_and_resync(void)
{
    size_t length = test_fixture_read("frames/fwdata_chunk.bin", fixture, sizeof(fixture));
    link_fwdata_chunk_t chunk;
    CHECK(link_decode_fwdata(fixture, length, &chunk));
    CHECK(chunk.transfer == 3 && chunk.offset == 65536 && chunk.length == 3);
    CHECK(chunk.data[0] == 0xAA && chunk.data[2] == 0xCC);

    length = test_fixture_read("frames/fwdata_ack.bin", fixture, sizeof(fixture));
    size_t made = link_encode_fwdata_ack(encoded, sizeof(encoded), 3, 65536, 0);
    CHECK(made == length && memcmp(encoded, fixture, length) == 0);

    length = test_fixture_read("frames/resync_reattached.bin", fixture, sizeof(fixture));
    made = link_encode_resync(encoded, sizeof(encoded), 0, LINK_RESYNC_REATTACHED);
    CHECK(made == length && memcmp(encoded, fixture, length) == 0);
}

static void encoders_refuse_a_small_buffer(void)
{
    CHECK(link_encode_cmd_response(encoded, 7, 1, 0, NULL, 0) == 0);
    CHECK(link_encode_resync(encoded, LINK_RESYNC_BYTES - 1, 0, 0) == 0);
    CHECK(link_encode_fwdata_ack(encoded, LINK_FWDATA_ACK_BYTES - 1, 0, 0, 0) == 0);
    const uint8_t packet[1] = {0x02};
    CHECK(link_encode_notify(encoded, LINK_NOTIFY_HEADER_BYTES, 0, 0, 0, packet, 1) == 0);
}

void test_link_frames(void)
{
    RUN(decodes_the_set_master_volume_request);
    RUN(decodes_the_get_platform_request);
    RUN(rejects_malformed_requests);
    RUN(encodes_cmd_responses);
    RUN(encodes_a_notify);
    RUN(encodes_an_empty_notify_without_a_packet);
    RUN(encodes_a_poll);
    RUN(handles_fwdata_and_resync);
    RUN(encoders_refuse_a_small_buffer);
}
