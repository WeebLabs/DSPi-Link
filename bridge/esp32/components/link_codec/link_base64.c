/*
 * link_base64.c - see link_base64.h.
 *
 * Portable: no ESP-IDF headers.
 */

#include "link_base64.h"

static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t link_base64_encoded_length(size_t length)
{
    return (length + 2) / 3 * 4;
}

size_t link_base64_encode(const uint8_t *data, size_t length, char *out)
{
    size_t written = 0;
    for (size_t i = 0; i < length; i += 3) {
        const size_t left = length - i;
        const uint32_t group = ((uint32_t)data[i] << 16) |
                               (left > 1 ? (uint32_t)data[i + 1] << 8 : 0) |
                               (left > 2 ? (uint32_t)data[i + 2] : 0);
        out[written++] = ALPHABET[(group >> 18) & 0x3F];
        out[written++] = ALPHABET[(group >> 12) & 0x3F];
        out[written++] = left > 1 ? ALPHABET[(group >> 6) & 0x3F] : '=';
        out[written++] = left > 2 ? ALPHABET[group & 0x3F] : '=';
    }
    out[written] = '\0';
    return written;
}
