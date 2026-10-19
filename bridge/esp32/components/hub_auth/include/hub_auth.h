/*
 * hub_auth.h - the bridge's optional PIN, its paired clients and its rate
 * limits.
 *
 * Implements Link spec 6.2 for a fixed PIN, as bridge spec 8.9 describes:
 *
 *   - The PIN is stored only as a salted PBKDF2-HMAC-SHA256 hash.
 *   - Pairing with the PIN issues a token: AUTH_TOKEN_BYTES random bytes as
 *     base64url. Only the token's SHA-256 is kept, with the client's name,
 *     when it was created and when it was last seen.
 *   - Five failures from one address within a minute lock that address out
 *     for a minute; twenty from all addresses together lock out pairing for
 *     everyone. A fixed PIN is never invalidated by failures.
 *   - Changing the PIN or turning it off forgets every client.
 *   - If the random source or a hash fails, nothing is set, issued or
 *     accepted: every function answers HUB_AUTH_UNAVAILABLE instead.
 *
 * Portable: no ESP-IDF headers. Randomness, hashing, time and saving come
 * through hub_auth_io_t, so the host tests can drive it with fakes; the
 * hub task supplies PSA crypto and NVS. Every function here is called only
 * by the hub task, which owns the hub_auth_t, so nothing needs locking.
 */

#ifndef HUB_AUTH_H
#define HUB_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_config.h"

/* SHA-256 output, the size of every hash kept here. */
#define HUB_AUTH_HASH_BYTES 32

/* AUTH_TOKEN_BYTES as unpadded base64url: four characters per three bytes,
 * rounded up. */
#define HUB_AUTH_TOKEN_CHARS ((AUTH_TOKEN_BYTES * 4 + 2) / 3)

/* One paired client (Link spec 7.3, auth.list). */
typedef struct {
    bool used;
    uint16_t cid; /* Client id, never 0 */
    uint8_t token_hash[HUB_AUTH_HASH_BYTES];
    char name[AUTH_CLIENT_NAME_BYTES];
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

    /* Fills `out` with `length` bytes from a true random source. False if
     * the source failed. */
    bool (*random)(void *context, uint8_t *out, size_t length);

    /* SHA-256 of `data`. False if the hash could not be computed. */
    bool (*sha256)(void *context, const uint8_t *data, size_t length,
                   uint8_t out[HUB_AUTH_HASH_BYTES]);

    /* PBKDF2-HMAC-SHA256 of `pin` with `salt`, AUTH_PIN_PBKDF2_ITERATIONS
     * rounds, HUB_AUTH_HASH_BYTES out. False if it could not be computed. */
    bool (*pbkdf2)(void *context, const char *pin, const uint8_t salt[AUTH_PIN_SALT_BYTES],
                   uint8_t out[HUB_AUTH_HASH_BYTES]);

    /* Milliseconds from any fixed point, for the rate limits; wraps. */
    uint32_t (*now_ms)(void *context);

    /* Seconds since 1970, or 0 while the clock is not set (no SNTP yet). */
    uint32_t (*unix_time)(void *context);

    /* Saves the whole store. Called after every change worth keeping; a
     * failure is the I/O's to log, since the store in memory stays right. */
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
    hub_auth_address_t addresses[AUTH_TRACKED_ADDRESSES];
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
    HUB_AUTH_INVALID_PIN,  /* hub_auth_set_pin(): not AUTH_PIN_DIGITS digits */
    HUB_AUTH_UNAVAILABLE,  /* The random source or a hash failed; nothing changed */
} hub_auth_result_t;

/* Starts `auth` from a saved store, or from an empty one if `saved` is
 * NULL. `io` is copied. Called once, before any other function. */
void hub_auth_init(hub_auth_t *auth, const hub_auth_io_t *io, const hub_auth_store_t *saved);

/* True when a PIN is set. */
bool hub_auth_pin_enabled(const hub_auth_t *auth);

/* True for exactly AUTH_PIN_DIGITS decimal digits. Safe from any task: it
 * touches no state. */
bool hub_auth_pin_is_valid(const char *pin);

/*
 * Turns the PIN on with `pin`, or off when `pin` is NULL. Either way every
 * paired client is forgotten (Link spec 6.2) and the store is saved.
 * Returns HUB_AUTH_OK, HUB_AUTH_INVALID_PIN for a PIN that is not
 * AUTH_PIN_DIGITS digits, or HUB_AUTH_UNAVAILABLE if the salt or hash could
 * not be made; on either failure nothing changes.
 */
hub_auth_result_t hub_auth_set_pin(hub_auth_t *auth, const char *pin);

/*
 * Checks `pin`, typed by a client at IPv4 `address`, against the PIN and
 * the rate limits, without pairing. Used by the setup page (bridge spec
 * 9.2). Returns HUB_AUTH_OK, HUB_AUTH_WRONG (also when no PIN is set),
 * HUB_AUTH_RATE_LIMITED, or HUB_AUTH_UNAVAILABLE when the hash failed,
 * which counts as no failure.
 */
hub_auth_result_t hub_auth_check_pin(hub_auth_t *auth, uint32_t address, const char *pin);

/*
 * auth.pair: checks `pin` as hub_auth_check_pin() does and, if it is right,
 * pairs a client called `name` (NULL for none). On HUB_AUTH_OK `token`
 * receives the new token, NUL-terminated, and `cid` its client id. When the
 * list is full, the client seen least recently is replaced. Other results
 * are those of hub_auth_check_pin() and hub_auth_issue().
 */
hub_auth_result_t hub_auth_pair(hub_auth_t *auth, uint32_t address, const char *pin,
                                const char *name, char token[HUB_AUTH_TOKEN_CHARS + 1],
                                uint16_t *cid);

/*
 * Issues a token for a client called `name` without a PIN check, for the
 * session that just set the PIN through hub.auth.set (Link spec 7.12).
 * Returns HUB_AUTH_OK with `token` and `cid` filled in, or
 * HUB_AUTH_UNAVAILABLE, leaving the client list as it was.
 */
hub_auth_result_t hub_auth_issue(hub_auth_t *auth, const char *name,
                                 char token[HUB_AUTH_TOKEN_CHARS + 1], uint16_t *cid);

/*
 * auth.token: finds the client that `token`, presented from IPv4
 * `address`, belongs to, and records it as seen. Returns HUB_AUTH_OK with
 * `cid` filled in, HUB_AUTH_WRONG for an unknown token (counted as a
 * failure), HUB_AUTH_RATE_LIMITED, or HUB_AUTH_UNAVAILABLE when the hash
 * failed.
 */
hub_auth_result_t hub_auth_token(hub_auth_t *auth, uint32_t address, const char *token,
                                 uint16_t *cid);

/* Records that client `cid` was active. Saves at most once per
 * AUTH_LAST_SEEN_SAVE_INTERVAL_MS per client, to spare flash. An unknown
 * `cid` is ignored. */
void hub_auth_touch(hub_auth_t *auth, uint16_t cid);

/* Forgets client `cid` and saves. False if there is no such client. */
bool hub_auth_revoke(hub_auth_t *auth, uint16_t cid);

/* The paired clients, for auth.list: AUTH_MAX_PAIRED_CLIENTS entries,
 * written to `count`. Entries with `used` false are empty. The array stays
 * valid until the next call that changes the list. */
const hub_auth_client_t *hub_auth_clients(const hub_auth_t *auth, size_t *count);

#endif /* HUB_AUTH_H */
