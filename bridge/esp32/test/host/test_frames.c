/*
 * test_frames.c - see test_frames.h.
 */

#include "test_frames.h"

#include <string.h>

/* A second, deliberately simple CRC-16/CCITT-FALSE, so the tests do not
 * only check dspi_frame.c against itself. */
uint16_t test_reference_crc(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            const int input_bit = (data[i] >> bit) & 1;
            const int top_bit = (crc >> 15) & 1;
            crc = (uint16_t)(crc << 1);
            if (input_bit != top_bit) {
                crc ^= 0x1021;
            }
        }
    }
    return crc;
}

size_t test_build_device_frame(uint8_t type, uint8_t status, const uint8_t *payload, size_t length,
                               uint8_t *out)
{
    out[0] = 0xA5;
    out[1] = type;
    out[2] = status;
    out[3] = (uint8_t)(length & 0xFF);
    out[4] = (uint8_t)(length >> 8);
    if (length > 0) {
        memcpy(&out[5], payload, length);
    }
    const uint16_t crc = test_reference_crc(&out[1], 4 + length);
    out[5 + length] = (uint8_t)(crc & 0xFF);
    out[6 + length] = (uint8_t)(crc >> 8);
    return 7 + length;
}
