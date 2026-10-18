/*
 * test_frames.h - builds frames the way the DSPi firmware sends them, for
 * tests. Written independently of dspi_frame.c's parser so the two check
 * each other.
 */

#ifndef TEST_FRAMES_H
#define TEST_FRAMES_H

#include <stddef.h>
#include <stdint.h>

/* CRC-16/CCITT-FALSE written independently of dspi_frame.c. */
uint16_t test_reference_crc(const uint8_t *data, size_t length);

/* Writes A5 type status length(2) payload crc(2) into `out` and returns its
 * length. `out` needs length + 7 bytes. */
size_t test_build_device_frame(uint8_t type, uint8_t status, const uint8_t *payload, size_t length,
                               uint8_t *out);

#endif /* TEST_FRAMES_H */
