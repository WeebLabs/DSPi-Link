/*
 * fake_crypto.c - see fake_crypto.h.
 */

#include "fake_crypto.h"

#include <string.h>

static uint32_t random_state = 1;
bool fake_random_fails;
bool fake_sha256_fails;
bool fake_pbkdf2_fails;

void fake_crypto_reset(void)
{
    random_state = 1;
    fake_random_fails = false;
    fake_sha256_fails = false;
    fake_pbkdf2_fails = false;
}

/* A linear congruential generator: repeatable, and no repeats in any test's
 * lifetime, unlike a byte counter that wraps after 256 bytes. */
bool fake_random(void *context, uint8_t *out, size_t length)
{
    (void)context;
    if (fake_random_fails) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        random_state = random_state * 1664525u + 1013904223u;
        out[i] = (uint8_t)(random_state >> 24);
    }
    return true;
}

/* FNV-1a spread over HUB_AUTH_HASH_BYTES bytes. */
bool fake_sha256(void *context, const uint8_t *data, size_t length,
                 uint8_t out[HUB_AUTH_HASH_BYTES])
{
    (void)context;
    if (fake_sha256_fails) {
        return false;
    }
    uint32_t hash = 2166136261u;
    for (size_t round = 0; round < HUB_AUTH_HASH_BYTES; round++) {
        for (size_t i = 0; i < length; i++) {
            hash = (hash ^ data[i]) * 16777619u;
        }
        hash = (hash ^ (uint32_t)round) * 16777619u;
        out[round] = (uint8_t)(hash >> 24);
    }
    return true;
}

bool fake_pbkdf2(void *context, const char *pin, const uint8_t salt[AUTH_PIN_SALT_BYTES],
                 uint8_t out[HUB_AUTH_HASH_BYTES])
{
    if (fake_pbkdf2_fails) {
        return false;
    }
    uint8_t input[AUTH_PIN_DIGITS + AUTH_PIN_SALT_BYTES];
    memcpy(input, pin, AUTH_PIN_DIGITS);
    memcpy(input + AUTH_PIN_DIGITS, salt, AUTH_PIN_SALT_BYTES);
    return fake_sha256(context, input, sizeof(input), out);
}
