/*
 * hub_auth.c - the PIN, paired clients' tokens and the pairing rate limits
 * (see hub_auth.h for the rules). Kept apart from the hub so the security
 * logic is small enough to review on its own and tested with fake crypto.
 *
 * Implements Link spec 6.2 for a fixed PIN and bridge spec 8.9.
 *
 * Portable: no ESP-IDF headers.
 */

#include "hub_auth.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

static uint32_t now_ms(const hub_auth_t *auth)
{
    return auth->io.now_ms(auth->io.context);
}

static bool reached(uint32_t now, uint32_t when)
{
    return (int32_t)(now - when) >= 0;
}

/* Compares two hashes in time that does not depend on where they differ,
 * so response times reveal nothing about a stored hash. */
static bool same_hash(const uint8_t *a, const uint8_t *b)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < HUB_AUTH_HASH_BYTES; i++) {
        difference |= (uint8_t)(a[i] ^ b[i]);
    }
    return difference == 0;
}

/* AUTH_TOKEN_BYTES bytes as base64url without padding (RFC 4648 section
 * 5). */
static void encode_token(const uint8_t bytes[AUTH_TOKEN_BYTES], char out[HUB_AUTH_TOKEN_CHARS + 1])
{
    static const char ALPHABET[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t written = 0;
    for (size_t i = 0; i < AUTH_TOKEN_BYTES; i += 3) {
        const size_t left = AUTH_TOKEN_BYTES - i;
        const uint32_t group = ((uint32_t)bytes[i] << 16) |
                               (left > 1 ? (uint32_t)bytes[i + 1] << 8 : 0) |
                               (left > 2 ? (uint32_t)bytes[i + 2] : 0);
        out[written++] = ALPHABET[(group >> 18) & 0x3F];
        out[written++] = ALPHABET[(group >> 12) & 0x3F];
        if (left > 1) {
            out[written++] = ALPHABET[(group >> 6) & 0x3F];
        }
        if (left > 2) {
            out[written++] = ALPHABET[group & 0x3F];
        }
    }
    out[written] = '\0';
}

static void save(const hub_auth_t *auth)
{
    auth->io.save(auth->io.context, &auth->store);
}

/* ---------------------------------------------------------------------------
 * Rate limits (Link spec 6.2)
 * ------------------------------------------------------------------------- */

/* The record for `address`: its own, else an empty one, else the one whose
 * last failure is oldest, reused. */
static hub_auth_address_t *address_record(hub_auth_t *auth, uint32_t address)
{
    for (size_t i = 0; i < AUTH_TRACKED_ADDRESSES; i++) {
        if (auth->addresses[i].address == address) {
            return &auth->addresses[i];
        }
    }
    hub_auth_address_t *choice = &auth->addresses[0];
    for (size_t i = 0; i < AUTH_TRACKED_ADDRESSES; i++) {
        hub_auth_address_t *record = &auth->addresses[i];
        if (record->address == 0) {
            choice = record;
            break;
        }
        if ((int32_t)(record->last_failure_ms - choice->last_failure_ms) < 0) {
            choice = record;
        }
    }
    *choice = (hub_auth_address_t){.address = address};
    return choice;
}

/* True while `address` is locked out, or, when `include_global`, while
 * everyone is. Expired locks lift. The global lock guards the PIN alone: a
 * paired client's token is 32 random bytes nobody can guess, so refusing
 * tokens too would only let a stranger guessing PINs lock every paired app
 * out (Link spec 6.2). */
static bool locked_out(hub_auth_t *auth, uint32_t address, bool include_global)
{
    const uint32_t now = now_ms(auth);
    if (auth->global_locked && reached(now, auth->global_locked_until_ms)) {
        auth->global_locked = false;
        auth->global_failures = 0;
    }
    for (size_t i = 0; i < AUTH_TRACKED_ADDRESSES; i++) {
        hub_auth_address_t *record = &auth->addresses[i];
        if (record->address == address && record->locked) {
            if (reached(now, record->locked_until_ms)) {
                record->locked = false;
                record->failures = 0;
            } else {
                return true;
            }
        }
    }
    return include_global && auth->global_locked;
}

/* Counts one failure from `address`, locking out at the limits. */
static void note_failure(hub_auth_t *auth, uint32_t address)
{
    const uint32_t now = now_ms(auth);
    hub_auth_address_t *record = address_record(auth, address);
    if (record->failures == 0 || reached(now, record->window_start_ms + AUTH_FAILURE_WINDOW_MS)) {
        record->window_start_ms = now;
        record->failures = 0;
    }
    record->last_failure_ms = now;
    if (++record->failures >= AUTH_MAX_FAILURES_PER_ADDRESS) {
        record->locked = true;
        record->locked_until_ms = now + AUTH_LOCKOUT_MS;
    }

    if (auth->global_failures == 0 ||
        reached(now, auth->global_window_start_ms + AUTH_FAILURE_WINDOW_MS)) {
        auth->global_window_start_ms = now;
        auth->global_failures = 0;
    }
    if (++auth->global_failures >= AUTH_MAX_FAILURES_ALL_ADDRESSES) {
        auth->global_locked = true;
        auth->global_locked_until_ms = now + AUTH_LOCKOUT_MS;
    }
}

/* ---------------------------------------------------------------------------
 * The PIN
 * ------------------------------------------------------------------------- */

void hub_auth_init(hub_auth_t *auth, const hub_auth_io_t *io, const hub_auth_store_t *saved)
{
    memset(auth, 0, sizeof(*auth));
    auth->io = *io;
    if (saved != NULL) {
        auth->store = *saved;
    }
    if (auth->store.next_cid == 0) {
        auth->store.next_cid = 1;
    }
}

bool hub_auth_pin_enabled(const hub_auth_t *auth)
{
    return auth->store.pin_enabled;
}

bool hub_auth_pin_is_valid(const char *pin)
{
    if (pin == NULL || strlen(pin) != AUTH_PIN_DIGITS) {
        return false;
    }
    for (size_t i = 0; i < AUTH_PIN_DIGITS; i++) {
        if (pin[i] < '0' || pin[i] > '9') {
            return false;
        }
    }
    return true;
}

hub_auth_result_t hub_auth_set_pin(hub_auth_t *auth, const char *pin)
{
    if (pin != NULL && !hub_auth_pin_is_valid(pin)) {
        return HUB_AUTH_INVALID_PIN;
    }
    /* Made aside first, so a failed random source or hash leaves the old
     * PIN and every client in place. */
    uint8_t salt[AUTH_PIN_SALT_BYTES] = {0};
    uint8_t hash[HUB_AUTH_HASH_BYTES] = {0};
    if (pin != NULL && (!auth->io.random(auth->io.context, salt, sizeof(salt)) ||
                        !auth->io.pbkdf2(auth->io.context, pin, salt, hash))) {
        return HUB_AUTH_UNAVAILABLE;
    }

    hub_auth_store_t *store = &auth->store;
    memset(store->clients, 0, sizeof(store->clients)); /* Every token is revoked */
    memcpy(store->pin_salt, salt, sizeof(store->pin_salt));
    memcpy(store->pin_hash, hash, sizeof(store->pin_hash));
    store->pin_enabled = pin != NULL;
    save(auth);
    return HUB_AUTH_OK;
}

hub_auth_result_t hub_auth_check_pin(hub_auth_t *auth, uint32_t address, const char *pin)
{
    if (locked_out(auth, address, true)) {
        return HUB_AUTH_RATE_LIMITED;
    }
    uint8_t hash[HUB_AUTH_HASH_BYTES];
    const bool well_formed = hub_auth_pin_is_valid(pin);
    if (well_formed && !auth->io.pbkdf2(auth->io.context, pin, auth->store.pin_salt, hash)) {
        return HUB_AUTH_UNAVAILABLE; /* Not the client's fault, so not a failure */
    }
    if (!auth->store.pin_enabled || !well_formed || !same_hash(hash, auth->store.pin_hash)) {
        note_failure(auth, address);
        return HUB_AUTH_WRONG;
    }
    return HUB_AUTH_OK;
}

/* ---------------------------------------------------------------------------
 * Clients and tokens
 * ------------------------------------------------------------------------- */

/* A free entry, or the one seen least recently when all are used. */
static hub_auth_client_t *client_entry_for_new(hub_auth_t *auth)
{
    hub_auth_client_t *oldest = &auth->store.clients[0];
    for (size_t i = 0; i < AUTH_MAX_PAIRED_CLIENTS; i++) {
        hub_auth_client_t *client = &auth->store.clients[i];
        if (!client->used) {
            return client;
        }
        if (client->last_seen < oldest->last_seen) {
            oldest = client;
        }
    }
    return oldest;
}

static uint16_t next_cid(hub_auth_t *auth)
{
    const uint16_t cid = auth->store.next_cid++;
    if (auth->store.next_cid == 0) {
        auth->store.next_cid = 1; /* 0 means "no client" */
    }
    return cid;
}

hub_auth_result_t hub_auth_issue(hub_auth_t *auth, const char *name,
                                 char token[HUB_AUTH_TOKEN_CHARS + 1], uint16_t *cid)
{
    uint8_t secret[AUTH_TOKEN_BYTES];
    uint8_t token_hash[HUB_AUTH_HASH_BYTES];
    if (!auth->io.random(auth->io.context, secret, sizeof(secret))) {
        return HUB_AUTH_UNAVAILABLE;
    }
    encode_token(secret, token);
    if (!auth->io.sha256(auth->io.context, (const uint8_t *)token, HUB_AUTH_TOKEN_CHARS,
                         token_hash)) {
        return HUB_AUTH_UNAVAILABLE;
    }

    hub_auth_client_t *client = client_entry_for_new(auth);
    const uint32_t now = auth->io.unix_time(auth->io.context);
    *client =
        (hub_auth_client_t){.used = true, .cid = next_cid(auth), .created = now, .last_seen = now};
    memcpy(client->token_hash, token_hash, sizeof(client->token_hash));
    snprintf(client->name, sizeof(client->name), "%s", name != NULL ? name : "");
    *cid = client->cid;
    save(auth);
    return HUB_AUTH_OK;
}

hub_auth_result_t hub_auth_pair(hub_auth_t *auth, uint32_t address, const char *pin,
                                const char *name, char token[HUB_AUTH_TOKEN_CHARS + 1],
                                uint16_t *cid)
{
    const hub_auth_result_t checked = hub_auth_check_pin(auth, address, pin);
    if (checked != HUB_AUTH_OK) {
        return checked;
    }
    return hub_auth_issue(auth, name, token, cid);
}

hub_auth_result_t hub_auth_token(hub_auth_t *auth, uint32_t address, const char *token,
                                 uint16_t *cid)
{
    if (locked_out(auth, address, false)) {
        return HUB_AUTH_RATE_LIMITED;
    }
    if (token != NULL && strlen(token) == HUB_AUTH_TOKEN_CHARS) {
        uint8_t hash[HUB_AUTH_HASH_BYTES];
        if (!auth->io.sha256(auth->io.context, (const uint8_t *)token, HUB_AUTH_TOKEN_CHARS,
                             hash)) {
            return HUB_AUTH_UNAVAILABLE;
        }
        for (size_t i = 0; i < AUTH_MAX_PAIRED_CLIENTS; i++) {
            const hub_auth_client_t *client = &auth->store.clients[i];
            if (client->used && same_hash(client->token_hash, hash)) {
                *cid = client->cid;
                hub_auth_touch(auth, client->cid);
                return HUB_AUTH_OK;
            }
        }
    }
    note_failure(auth, address);
    return HUB_AUTH_WRONG;
}

void hub_auth_touch(hub_auth_t *auth, uint16_t cid)
{
    for (size_t i = 0; i < AUTH_MAX_PAIRED_CLIENTS; i++) {
        hub_auth_client_t *client = &auth->store.clients[i];
        if (!client->used || client->cid != cid) {
            continue;
        }
        const uint32_t unix_now = auth->io.unix_time(auth->io.context);
        if (unix_now != 0) {
            client->last_seen = unix_now;
        }
        const uint32_t now = now_ms(auth);
        if (reached(now, auth->last_saved_ms[i] + AUTH_LAST_SEEN_SAVE_INTERVAL_MS)) {
            auth->last_saved_ms[i] = now;
            save(auth);
        }
        return;
    }
}

bool hub_auth_revoke(hub_auth_t *auth, uint16_t cid)
{
    for (size_t i = 0; i < AUTH_MAX_PAIRED_CLIENTS; i++) {
        hub_auth_client_t *client = &auth->store.clients[i];
        if (client->used && client->cid == cid) {
            memset(client, 0, sizeof(*client));
            save(auth);
            return true;
        }
    }
    return false;
}

const hub_auth_client_t *hub_auth_clients(const hub_auth_t *auth, size_t *count)
{
    *count = AUTH_MAX_PAIRED_CLIENTS;
    return auth->store.clients;
}
