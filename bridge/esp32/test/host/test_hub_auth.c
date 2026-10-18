/*
 * test_hub_auth.c - the fixed PIN, tokens and rate limits (Link spec 6.2,
 * bridge spec 8.9).
 *
 * The hashing here is a stand-in: these tests check what hub_auth does with
 * hashes, not the hashes themselves, which mbedtls provides on the device.
 */

#include <string.h>

#include "bridge_config.h"
#include "check.h"
#include "fake_crypto.h"
#include "hub_auth.h"

static hub_auth_t auth;
static uint32_t clock_ms;
static uint32_t clock_unix;
static unsigned saves;
static uint32_t fake_now(void *context)
{
    (void)context;
    return clock_ms;
}

static uint32_t fake_unix(void *context)
{
    (void)context;
    return clock_unix;
}

static void fake_save(void *context, const hub_auth_store_t *store)
{
    (void)context;
    (void)store;
    saves++;
}

static void start(void)
{
    clock_ms = 1000;
    clock_unix = 1800000000;
    saves = 0;
    fake_crypto_reset();
    const hub_auth_io_t io = {
        .random = fake_random,
        .sha256 = fake_sha256,
        .pbkdf2 = fake_pbkdf2,
        .now_ms = fake_now,
        .unix_time = fake_unix,
        .save = fake_save,
    };
    hub_auth_init(&auth, &io, NULL);
}

#define HOME 0x0A01A8C0u  /* 192.168.1.10 */
#define OTHER 0x0B01A8C0u /* 192.168.1.11 */

static void the_pin_is_off_until_set(void)
{
    start();
    CHECK(!hub_auth_pin_enabled(&auth));
    CHECK(!hub_auth_set_pin(&auth, "12345"));  /* Too short */
    CHECK(!hub_auth_set_pin(&auth, "12345a")); /* Not digits */
    CHECK(!hub_auth_pin_enabled(&auth));
    CHECK(hub_auth_set_pin(&auth, "482913"));
    CHECK(hub_auth_pin_enabled(&auth));
    CHECK(memcmp(auth.store.pin_hash, "482913", 6) != 0); /* Only a hash is kept */
    CHECK(saves == 1);
}

static void pairing_issues_a_token_that_works(void)
{
    start();
    hub_auth_set_pin(&auth, "482913");
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t cid = 0;
    CHECK(hub_auth_pair(&auth, HOME, "482913", "Living Room iPad", token, &cid) == HUB_AUTH_OK);
    CHECK(strlen(token) == HUB_AUTH_TOKEN_CHARS);
    CHECK(strspn(token, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") ==
          HUB_AUTH_TOKEN_CHARS);
    CHECK(cid != 0);

    uint16_t found = 0;
    CHECK(hub_auth_token(&auth, HOME, token, &found) == HUB_AUTH_OK);
    CHECK(found == cid);

    token[0] = token[0] == 'A' ? 'B' : 'A';
    CHECK(hub_auth_token(&auth, HOME, token, &found) == HUB_AUTH_WRONG);
}

static void five_failures_lock_out_one_address(void)
{
    start();
    hub_auth_set_pin(&auth, "482913");
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t cid;
    for (int i = 0; i < AUTH_MAX_FAILURES_PER_ADDRESS; i++) {
        CHECK(hub_auth_pair(&auth, HOME, "000000", "x", token, &cid) == HUB_AUTH_WRONG);
    }
    CHECK(hub_auth_pair(&auth, HOME, "482913", "x", token, &cid) == HUB_AUTH_RATE_LIMITED);
    CHECK(hub_auth_pair(&auth, OTHER, "482913", "x", token, &cid) == HUB_AUTH_OK);

    clock_ms += AUTH_LOCKOUT_MS;
    CHECK(hub_auth_pair(&auth, HOME, "482913", "x", token, &cid) == HUB_AUTH_OK);
}

static void a_fixed_pin_survives_failures(void)
{
    start();
    hub_auth_set_pin(&auth, "482913");
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t cid;
    for (int i = 0; i < AUTH_MAX_FAILURES_PER_ADDRESS - 1; i++) {
        hub_auth_pair(&auth, HOME, "999999", "x", token, &cid);
    }
    CHECK(hub_auth_pair(&auth, HOME, "482913", "x", token, &cid) == HUB_AUTH_OK);
}

static void many_addresses_together_lock_out_everyone(void)
{
    start();
    hub_auth_set_pin(&auth, "482913");
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t cid;
    for (uint32_t i = 0; i < AUTH_MAX_FAILURES_ALL_ADDRESSES; i++) {
        hub_auth_pair(&auth, 0x01000000u + i, "000000", "x", token, &cid);
    }
    CHECK(hub_auth_pair(&auth, HOME, "482913", "x", token, &cid) == HUB_AUTH_RATE_LIMITED);
    clock_ms += AUTH_LOCKOUT_MS;
    CHECK(hub_auth_pair(&auth, HOME, "482913", "x", token, &cid) == HUB_AUTH_OK);
}

static void changing_the_pin_forgets_every_client(void)
{
    start();
    hub_auth_set_pin(&auth, "482913");
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t cid;
    hub_auth_pair(&auth, HOME, "482913", "x", token, &cid);
    hub_auth_set_pin(&auth, "135790");
    CHECK(hub_auth_token(&auth, HOME, token, &cid) == HUB_AUTH_WRONG);
    CHECK(hub_auth_pair(&auth, HOME, "482913", "x", token, &cid) == HUB_AUTH_WRONG);
    CHECK(hub_auth_pair(&auth, HOME, "135790", "x", token, &cid) == HUB_AUTH_OK);

    hub_auth_set_pin(&auth, NULL);
    CHECK(!hub_auth_pin_enabled(&auth));
    CHECK(hub_auth_token(&auth, HOME, token, &cid) == HUB_AUTH_WRONG);
}

static void clients_can_be_revoked(void)
{
    start();
    hub_auth_set_pin(&auth, "482913");
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t cid;
    hub_auth_pair(&auth, HOME, "482913", "Phone", token, &cid);
    CHECK(hub_auth_revoke(&auth, cid));
    CHECK(!hub_auth_revoke(&auth, cid));
    CHECK(hub_auth_token(&auth, HOME, token, &cid) == HUB_AUTH_WRONG);
}

static void a_full_list_replaces_the_stalest_client(void)
{
    start();
    hub_auth_set_pin(&auth, "482913");
    char first[HUB_AUTH_TOKEN_CHARS + 1];
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t cid;
    hub_auth_pair(&auth, HOME, "482913", "Oldest", first, &cid);
    for (int i = 1; i < AUTH_MAX_PAIRED_CLIENTS; i++) {
        clock_unix += 10;
        hub_auth_pair(&auth, HOME, "482913", "Newer", token, &cid);
    }
    clock_unix += 10;
    CHECK(hub_auth_pair(&auth, HOME, "482913", "Newest", token, &cid) == HUB_AUTH_OK);
    CHECK(hub_auth_token(&auth, HOME, first, &cid) == HUB_AUTH_WRONG);
    CHECK(hub_auth_token(&auth, HOME, token, &cid) == HUB_AUTH_OK);
}

static void last_seen_is_saved_at_most_hourly(void)
{
    start();
    hub_auth_set_pin(&auth, "482913");
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t cid;
    hub_auth_pair(&auth, HOME, "482913", "x", token, &cid);
    hub_auth_touch(&auth, cid); /* The first touch saves */
    const unsigned before = saves;
    for (int i = 0; i < 10; i++) {
        clock_ms += 60000;
        hub_auth_touch(&auth, cid);
    }
    CHECK(saves == before);
    clock_ms += AUTH_LAST_SEEN_SAVE_INTERVAL_MS;
    hub_auth_touch(&auth, cid);
    CHECK(saves == before + 1);
}

void test_hub_auth(void)
{
    RUN(the_pin_is_off_until_set);
    RUN(pairing_issues_a_token_that_works);
    RUN(five_failures_lock_out_one_address);
    RUN(a_fixed_pin_survives_failures);
    RUN(many_addresses_together_lock_out_everyone);
    RUN(changing_the_pin_forgets_every_client);
    RUN(clients_can_be_revoked);
    RUN(a_full_list_replaces_the_stalest_client);
    RUN(last_seen_is_saved_at_most_hourly);
}
