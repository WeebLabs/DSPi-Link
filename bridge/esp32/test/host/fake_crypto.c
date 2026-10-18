/*
 * fake_crypto.c - see fake_crypto.h.
 */

#include "fake_crypto.h"

#include <string.h>

static uint32_t random_state = 1;

void fake_crypto_reset(void)
{
    random_state = 1;
}

/* A linear congruential generator: repeatable, and no repeats in any test's
 * lifetime, unlike a byte counter that wraps after 256 bytes. */
void fake_random(void *context, uint8_t *out, size_t length)
{
    (void)context;
    for (size_t i = 0; i < length; i++) {
        random_state = random_state * 1664525u + 1013904223u;
        out[i] = (uint8_t)(random_state >> 24);
    }
}

/* FNV-1a spread over 32 bytes. */
void fake_sha256(void *context, const uint8_t *data, size_t length, uint8_t out[32])
{
    (void)context;
    uint32_t hash = 2166136261u;
    for (size_t round = 0; round < 32; round++) {
        for (size_t i = 0; i < length; i++) {
            hash = (hash ^ data[i]) * 16777619u;
        }
        hash = (hash ^ (uint32_t)round) * 16777619u;
        out[round] = (uint8_t)(hash >> 24);
    }
}

void fake_pbkdf2(void *context, const char *pin, const uint8_t salt[AUTH_PIN_SALT_BYTES],
                 uint8_t out[32])
{
    uint8_t input[AUTH_PIN_DIGITS + AUTH_PIN_SALT_BYTES];
    memcpy(input, pin, AUTH_PIN_DIGITS);
    memcpy(input + AUTH_PIN_DIGITS, salt, AUTH_PIN_SALT_BYTES);
    fake_sha256(context, input, sizeof(input), out);
}
