/*
 * fake_crypto.h - stand-ins for the random source, SHA-256 and PBKDF2 that
 * hub_auth uses, for host tests. They behave like the real thing where the
 * tests care (deterministic, different inputs give different outputs) but
 * are not cryptography; mbedtls provides that on the device.
 */

#ifndef FAKE_CRYPTO_H
#define FAKE_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#include "hub_auth.h"

/* Restarts the random sequence, so each test sees the same values. */
void fake_crypto_reset(void);

void fake_random(void *context, uint8_t *out, size_t length);
void fake_sha256(void *context, const uint8_t *data, size_t length, uint8_t out[32]);
void fake_pbkdf2(void *context, const char *pin, const uint8_t salt[AUTH_PIN_SALT_BYTES],
                 uint8_t out[32]);

#endif /* FAKE_CRYPTO_H */
