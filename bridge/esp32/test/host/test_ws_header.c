/*
 * test_ws_header.c - WebSocket frame headers (RFC 6455 section 5.2).
 */

#include "check.h"
#include "ws_header.h"

static void short_frames_have_a_two_byte_header(void)
{
    uint8_t header[WS_HEADER_MAX_BYTES];
    CHECK(ws_header_encode(WS_OPCODE_TEXT, 5, header) == 2);
    CHECK(header[0] == 0x81 && header[1] == 5); /* RFC 6455 5.7: "Hello" */
    CHECK(ws_header_encode(WS_OPCODE_BINARY, 125, header) == 2);
    CHECK(header[0] == 0x82 && header[1] == 125);
}

static void medium_frames_carry_a_16_bit_length(void)
{
    uint8_t header[WS_HEADER_MAX_BYTES];
    CHECK(ws_header_encode(WS_OPCODE_BINARY, 126, header) == 4);
    CHECK(header[1] == 126 && header[2] == 0 && header[3] == 126);
    CHECK(ws_header_encode(WS_OPCODE_TEXT, 8331, header) == 4); /* A snapshot reply */
    CHECK(header[2] == 0x20 && header[3] == 0x8B);
    CHECK(ws_header_encode(WS_OPCODE_TEXT, 65535, header) == 4);
}

static void long_frames_carry_a_64_bit_length(void)
{
    uint8_t header[WS_HEADER_MAX_BYTES];
    CHECK(ws_header_encode(WS_OPCODE_BINARY, 65536, header) == 10);
    CHECK(header[1] == 127);
    CHECK(header[2] == 0 && header[7] == 1 && header[8] == 0 && header[9] == 0);
}

static void control_frames_are_final(void)
{
    uint8_t header[WS_HEADER_MAX_BYTES];
    CHECK(ws_header_encode(WS_OPCODE_PING, 0, header) == 2);
    CHECK(header[0] == 0x89 && header[1] == 0);
    CHECK(ws_header_encode(WS_OPCODE_CLOSE, 2, header) == 2);
    CHECK(header[0] == 0x88);
}

void test_ws_header(void)
{
    RUN(short_frames_have_a_two_byte_header);
    RUN(medium_frames_carry_a_16_bit_length);
    RUN(long_frames_carry_a_64_bit_length);
    RUN(control_frames_are_final);
}
