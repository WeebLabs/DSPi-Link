/*
 * hub_auth.h - the bridge's optional PIN, its paired clients and its rate
 * limits.
 *
 * Implements Link spec 6.2 for a fixed PIN, as bridge spec 8.9 describes:
 *
 *   - The PIN is stored only as a salted PBKDF2-HMAC-SHA256 hash.
 *   - Pairing with the PIN issues a token: 32 random bytes as base64url.
 *     Only the token's SHA-256 is kept, with the client's name, when it was
 *     created and when it was last seen.
 *   - Five failures from one address within a minute lock that address out
 *     for a minute; twenty from all addresses together lock out pairing for
 *     everyone. A fixed PIN is never invalidated by failures.
 *   - Changing the PIN or turning it off forgets every client.
 *
 * Portable: no ESP-IDF headers. Randomness, hashing, time and saving come
 * through hub_auth_io_t, so the host tests can drive it with fakes; the
 * hub task supplies mbedtls and NVS. Owned by the hub task.
 */

#ifndef HUB_AUTH_H
#define HUB_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_config.h"

#define HUB_AUTH_HASH_BYTES 32
#define HUB_AUTH_TOKEN_CHARS 43 /* 32 bytes as unpadded base64url */
#define HUB_AUTH_NAME_BYTES 64
#define HUB_AUTH_TRACKED_ADDRESSES 8 /* Addresses whose failures are counted */

/* One paired client (Link spec 7.3, auth.list). */
typedef struct {
    bool used;
    uint16_t cid; /* Client id, never 0 */
    uint8_t token_hash[HUB_AUTH_HASH_BYTES];
    char name[HUB_AUTH_NAME_BYTES];
    uint32_t created;   /* Unix time, or 0 if the clock was not set */
    uint32_t last_seen; /* Unix time, or 0 */
} hub_auth_client_t;

/* Everything hub_auth saves (the `auth` namespace of bridge spec 11). */
typedef struct {
    bool pin_enabled;
    uint8_t pin_hash[HUB_AUTH_HASH_BYTES];
    uint8_t pin_salt[AUTH_PIN_SALT_BYTES];
    uint16_t next_cid;
    hub_auth_client_t clients[AUTH_MAX_PAIRED_CLIENTS];
} hub_auth_store_t;

typedef struct {
    void *context;

    /* Fills `out` with random bytes from a true random source. */
    void (*random)(void *context, uint8_t *out, size_t length);

    /* SHA-256 of `data`. */
    void (*sha256)(void *context, const uint8_t *data, size_t length,
                   uint8_t out[HUB_AUTH_HASH_BYTES]);

    /* PBKDF2-HMAC-SHA256 of `pin` with `salt`, AUTH_PIN_PBKDF2_ITERATIONS
     * rounds, 32 bytes out. */
    void (*pbkdf2)(void *context, const char *pin, const uint8_t salt[AUTH_PIN_SALT_BYTES],
                   uint8_t out[HUB_AUTH_HASH_BYTES]);

    uint32_t (*now_ms)(void *context);

    /* Seconds since 1970, or 0 while the clock is not set (no SNTP yet). */
    uint32_t (*unix_time)(void *context);

    /* Saves the whole store. Called after every change worth keeping. */
    void (*save)(void *context, const hub_auth_store_t *store);
} hub_auth_io_t;

/* Failures from one address (Link spec 6.2). */
typedef struct {
    uint32_t address; /* IPv4, 0 for an empty entry */
    uint32_t window_start_ms;
    uint8_t failures;
    bool locked;
    uint32_t locked_until_ms;
    uint32_t last_failure_ms; /* For replacing the stalest entry */
} hub_auth_address_t;

/* State. Treat as opaque. */
typedef struct {
    hub_auth_io_t io;
    hub_auth_store_t store;
    hub_auth_address_t addresses[HUB_AUTH_TRACKED_ADDRESSES];
    uint32_t last_saved_ms[AUTH_MAX_PAIRED_CLIENTS]; /* last_seen saves, per client */
    uint32_t global_window_start_ms;
    uint8_t global_failures;
    bool global_locked;
    uint32_t global_locked_until_ms;
} hub_auth_t;

typedef enum {
    HUB_AUTH_OK,
    HUB_AUTH_WRONG,        /* Wrong PIN or unknown token */
    HUB_AUTH_RATE_LIMITED, /* Too many failures; try again in a minute */
    HUB_AUTH_FULL,         /* No room for a client and none to replace */
} hub_auth_result_t;

/* Starts from a saved store, or from an empty one if `saved` is NULL. */
void hub_auth_init(hub_auth_t *auth, const hub_auth_io_t *io, const hub_auth_store_t *saved);

bool hub_auth_pin_enabled(const hub_auth_t *auth);

/* True for exactly AUTH_PIN_DIGITS decimal digits. */
bool hub_auth_pin_is_valid(const char *pin);

/*
 * Turns the PIN on with `pin`, or off when `pin` is NULL. Either way every
 * paired client is forgotten (Link spec 6.2). Returns false, changing
 * nothing, for a PIN that is not AUTH_PIN_DIGITS digits.
 */
bool hub_auth_set_pin(hub_auth_t *auth, const char *pin);

/* Checks the PIN from `address` against the rate limits, without pairing.
 * Used by the setup page in FALLBACK (bridge spec 9.2). */
hub_auth_result_t hub_auth_check_pin(hub_auth_t *auth, uint32_t address, const char *pin);

/*
 * auth.pair: checks the PIN and pairs a client called `name`. On success
 * `token` receives the new token, NUL-terminated, and `cid` its client id.
 * When the list is full, the client seen least recently is replaced.
 */
hub_auth_result_t hub_auth_pair(hub_auth_t *auth, uint32_t address, const char *pin,
                                const char *name, char token[HUB_AUTH_TOKEN_CHARS + 1],
                                uint16_t *cid);

/* Issues a token for a client without a PIN check, for the session that
 * just set the PIN through hub.auth.set (Link spec 7.12). */
hub_auth_result_t hub_auth_issue(hub_auth_t *auth, const char *name,
                                 char token[HUB_AUTH_TOKEN_CHARS + 1], uint16_t *cid);

/* auth.token: finds the client a token belongs to. */
hub_auth_result_t hub_auth_token(hub_auth_t *auth, uint32_t address, const char *token,
                                 uint16_t *cid);

/* Records that client `cid` was active. Saves at most once per
 * AUTH_LAST_SEEN_SAVE_INTERVAL_MS per client, to spare flash. */
void hub_auth_touch(hub_auth_t *auth, uint16_t cid);

/* Forgets client `cid`. False if there is no such client. */
bool hub_auth_revoke(hub_auth_t *auth, uint16_t cid);

/* The paired clients, for auth.list. Entries with `used` false are empty. */
const hub_auth_client_t *hub_auth_clients(const hub_auth_t *auth, size_t *count);

#endif /* HUB_AUTH_H */
