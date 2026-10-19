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
    CHECK(hub_auth_set_pin(&auth, "12345") == HUB_AUTH_INVALID_PIN);  /* Too short */
    CHECK(hub_auth_set_pin(&auth, "12345a") == HUB_AUTH_INVALID_PIN); /* Not digits */
    CHECK(!hub_auth_pin_enabled(&auth));
    CHECK(hub_auth_set_pin(&auth, "482913") == HUB_AUTH_OK);
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
    CHECK(hub_auth_pair(&auth, HOME, "482913", "Living Room iPad", token, &cid, NULL) ==
          HUB_AUTH_OK);
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
        CHECK(hub_auth_pair(&auth, HOME, "000000", "x", token, &cid, NULL) == HUB_AUTH_WRONG);
    }
    CHECK(hub_auth_pair(&auth, HOME, "482913", "x", token, &cid, NULL) == HUB_AUTH_RATE_LIMITED);
    CHECK(hub_auth_pair(&auth, OTHER, "482913", "x", token, &cid, NULL) == HUB_AUTH_OK);

    clock_ms += AUTH_LOCKOUT_MS;
    CHECK(hub_auth_pair(&auth, HOME, "482913", "x", token, &cid, NULL) == HUB_AUTH_OK);
}

static void a_fixed_pin_survives_failures(void)
{
    start();
    hub_auth_set_pin(&auth, "482913");
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t cid;
    for (int i = 0; i < AUTH_MAX_FAILURES_PER_ADDRESS - 1; i++) {
        hub_auth_pair(&auth, HOME, "999999", "x", token, &cid, NULL);
    }
    CHECK(hub_auth_pair(&auth, HOME, "482913", "x", token, &cid, NULL) == HUB_AUTH_OK);
}

static void many_addresses_together_lock_out_everyone(void)
{
    start();
    hub_auth_set_pin(&auth, "482913");
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    char paired[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t cid;
    CHECK(hub_auth_pair(&auth, HOME, "482913", "x", paired, &cid, NULL) == HUB_AUTH_OK);
    for (uint32_t i = 0; i < AUTH_MAX_FAILURES_ALL_ADDRESSES; i++) {
        hub_auth_pair(&auth, 0x01000000u + i, "000000", "x", token, &cid, NULL);
    }
    CHECK(hub_auth_pair(&auth, HOME, "482913", "x", token, &cid, NULL) == HUB_AUTH_RATE_LIMITED);
    /* The lock is on guessing the PIN; a client already paired gets in. */
    CHECK(hub_auth_token(&auth, HOME, paired, &cid) == HUB_AUTH_OK);
    clock_ms += AUTH_LOCKOUT_MS;
    CHECK(hub_auth_pair(&auth, HOME, "482913", "x", token, &cid, NULL) == HUB_AUTH_OK);
}

static void changing_the_pin_forgets_every_client(void)
{
    start();
    hub_auth_set_pin(&auth, "482913");
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t cid;
    hub_auth_pair(&auth, HOME, "482913", "x", token, &cid, NULL);
    hub_auth_set_pin(&auth, "135790");
    CHECK(hub_auth_token(&auth, HOME, token, &cid) == HUB_AUTH_WRONG);
    CHECK(hub_auth_pair(&auth, HOME, "482913", "x", token, &cid, NULL) == HUB_AUTH_WRONG);
    CHECK(hub_auth_pair(&auth, HOME, "135790", "x", token, &cid, NULL) == HUB_AUTH_OK);

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
    hub_auth_pair(&auth, HOME, "482913", "Phone", token, &cid, NULL);
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
    hub_auth_pair(&auth, HOME, "482913", "Oldest", first, &cid, NULL);
    for (int i = 1; i < AUTH_MAX_PAIRED_CLIENTS; i++) {
        clock_unix += 10;
        hub_auth_pair(&auth, HOME, "482913", "Newer", token, &cid, NULL);
    }
    clock_unix += 10;
    CHECK(hub_auth_pair(&auth, HOME, "482913", "Newest", token, &cid, NULL) == HUB_AUTH_OK);
    CHECK(hub_auth_token(&auth, HOME, first, &cid) == HUB_AUTH_WRONG);
    CHECK(hub_auth_token(&auth, HOME, token, &cid) == HUB_AUTH_OK);
}

static void last_seen_is_saved_at_most_hourly(void)
{
    start();
    hub_auth_set_pin(&auth, "482913");
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t cid;
    hub_auth_pair(&auth, HOME, "482913", "x", token, &cid, NULL); /* Saves, last_seen too */
    unsigned before = saves;
    hub_auth_touch(&auth, cid);
    CHECK(saves == before); /* Saved with the pairing a moment ago */
    clock_ms += AUTH_LAST_SEEN_SAVE_INTERVAL_MS;
    hub_auth_touch(&auth, cid);
    CHECK(saves == before + 1); /* The first touch in a new hour saves */
    before = saves;
    for (int i = 0; i < 10; i++) {
        clock_ms += 60000;
        hub_auth_touch(&auth, cid);
    }
    CHECK(saves == before);
    clock_ms += AUTH_LAST_SEEN_SAVE_INTERVAL_MS;
    hub_auth_touch(&auth, cid);
    CHECK(saves == before + 1);
}

static void a_failed_hash_sets_no_pin(void)
{
    start();
    fake_pbkdf2_fails = true;
    CHECK(hub_auth_set_pin(&auth, "482913") == HUB_AUTH_UNAVAILABLE);
    CHECK(!hub_auth_pin_enabled(&auth));
    CHECK(saves == 0);

    /* A PIN already set stays as it was, and so do its clients. */
    fake_pbkdf2_fails = false;
    CHECK(hub_auth_set_pin(&auth, "482913") == HUB_AUTH_OK);
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t cid;
    CHECK(hub_auth_pair(&auth, HOME, "482913", "x", token, &cid, NULL) == HUB_AUTH_OK);
    fake_random_fails = true;
    CHECK(hub_auth_set_pin(&auth, "135790") == HUB_AUTH_UNAVAILABLE);
    fake_random_fails = false;
    CHECK(hub_auth_token(&auth, HOME, token, &cid) == HUB_AUTH_OK);
    CHECK(hub_auth_check_pin(&auth, HOME, "482913") == HUB_AUTH_OK);
}

static void a_failed_hash_accepts_nothing_and_counts_nothing(void)
{
    start();
    hub_auth_set_pin(&auth, "482913");
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t cid;
    CHECK(hub_auth_pair(&auth, HOME, "482913", "x", token, &cid, NULL) == HUB_AUTH_OK);

    fake_pbkdf2_fails = true;
    for (int i = 0; i < AUTH_MAX_FAILURES_PER_ADDRESS; i++) {
        CHECK(hub_auth_check_pin(&auth, HOME, "482913") == HUB_AUTH_UNAVAILABLE);
        CHECK(hub_auth_pair(&auth, HOME, "000000", "x", token, &cid, NULL) == HUB_AUTH_UNAVAILABLE);
    }
    fake_sha256_fails = true;
    CHECK(hub_auth_token(&auth, HOME, token, &cid) == HUB_AUTH_UNAVAILABLE);

    /* None of that was the client's fault, so nothing locked it out. */
    fake_crypto_reset();
    CHECK(hub_auth_check_pin(&auth, HOME, "482913") == HUB_AUTH_OK);
}

static void a_failed_random_source_issues_no_token(void)
{
    start();
    hub_auth_set_pin(&auth, "482913");
    const unsigned before = saves;
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t cid = 0;
    fake_random_fails = true;
    CHECK(hub_auth_issue(&auth, "x", token, &cid, NULL) == HUB_AUTH_UNAVAILABLE);
    CHECK(hub_auth_pair(&auth, HOME, "482913", "x", token, &cid, NULL) == HUB_AUTH_UNAVAILABLE);
    fake_random_fails = false;
    fake_sha256_fails = true;
    CHECK(hub_auth_issue(&auth, "x", token, &cid, NULL) == HUB_AUTH_UNAVAILABLE);
    CHECK(cid == 0 && saves == before);
    size_t count = 0;
    const hub_auth_client_t *clients = hub_auth_clients(&auth, &count);
    for (size_t i = 0; i < count; i++) {
        CHECK(!clients[i].used);
    }
}

static void lockouts_end_however_long_nobody_tries(void)
{
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t cid;
    const uint32_t thirty_days = 30u * 24 * 3600 * 1000;

    /* One address locked, then a month of silence, past the point where a
     * signed 32-bit difference turns negative. */
    start();
    hub_auth_set_pin(&auth, "482913");
    for (int i = 0; i < AUTH_MAX_FAILURES_PER_ADDRESS; i++) {
        hub_auth_check_pin(&auth, HOME, "000000");
    }
    clock_ms += thirty_days;
    CHECK(hub_auth_check_pin(&auth, HOME, "482913") == HUB_AUTH_OK);

    /* Everyone locked, then a month: tokens and the PIN both work. */
    start();
    hub_auth_set_pin(&auth, "482913");
    hub_auth_pair(&auth, HOME, "482913", "x", token, &cid, NULL);
    for (uint32_t i = 0; i < AUTH_MAX_FAILURES_ALL_ADDRESSES; i++) {
        hub_auth_check_pin(&auth, 0x01000000u + i, "000000");
    }
    clock_ms += thirty_days;
    CHECK(hub_auth_token(&auth, OTHER, token, &cid) == HUB_AUTH_OK);
    CHECK(hub_auth_check_pin(&auth, OTHER, "482913") == HUB_AUTH_OK);

    /* Four typos, then one more a month later: a new window, no lockout. */
    start();
    hub_auth_set_pin(&auth, "482913");
    for (int i = 0; i < AUTH_MAX_FAILURES_PER_ADDRESS - 1; i++) {
        hub_auth_check_pin(&auth, HOME, "000000");
    }
    clock_ms += thirty_days;
    hub_auth_check_pin(&auth, HOME, "000001");
    CHECK(hub_auth_check_pin(&auth, HOME, "482913") == HUB_AUTH_OK);

    /* With the hub ticking, a whole turn of the millisecond clock (49.7
     * days) cannot bring a lock back, even when the clock comes round to
     * just after the moment it was set. */
    start();
    hub_auth_set_pin(&auth, "482913");
    const uint32_t locked_at = clock_ms;
    for (int i = 0; i < AUTH_MAX_FAILURES_PER_ADDRESS; i++) {
        hub_auth_check_pin(&auth, HOME, "000000");
    }
    for (uint64_t passed = 0; passed < (1ull << 32); passed += 60000) {
        clock_ms += 60000;
        hub_auth_tick(&auth);
    }
    clock_ms = locked_at + AUTH_LOCKOUT_MS / 2;
    CHECK(hub_auth_check_pin(&auth, HOME, "482913") == HUB_AUTH_OK);
}

static void a_lockout_is_never_given_up_for_room(void)
{
    start();
    hub_auth_set_pin(&auth, "482913");
    uint16_t cid;
    for (int i = 0; i < AUTH_MAX_FAILURES_PER_ADDRESS; i++) {
        hub_auth_token(&auth, HOME, "wrong", &cid);
    }
    /* More addresses fail than there are records. */
    for (uint32_t i = 0; i < 2 * AUTH_TRACKED_ADDRESSES; i++) {
        clock_ms += 10;
        hub_auth_token(&auth, 0x01000000u + i, "wrong", &cid);
    }
    CHECK(hub_auth_token(&auth, HOME, "wrong", &cid) == HUB_AUTH_RATE_LIMITED);

    /* An address of 0 (a client with no IPv4 address) is an address like
     * any other, not an empty record. */
    start();
    hub_auth_set_pin(&auth, "482913");
    for (int i = 0; i < AUTH_MAX_FAILURES_PER_ADDRESS; i++) {
        hub_auth_token(&auth, 0, "wrong", &cid);
    }
    hub_auth_token(&auth, OTHER, "wrong", &cid);
    CHECK(hub_auth_token(&auth, 0, "wrong", &cid) == HUB_AUTH_RATE_LIMITED);
    CHECK(hub_auth_check_pin(&auth, OTHER, "482913") == HUB_AUTH_OK);
}

static void names_are_cut_and_cleaned_into_utf8(void)
{
    char out[8];
    hub_auth_copy_name(out, sizeof(out), "abc");
    CHECK_STR_EQ(out, "abc");
    hub_auth_copy_name(out, sizeof(out),
                       "abcde\xE6\x97\xA5"); /* 8 bytes: the 3-byte one won't fit */
    CHECK_STR_EQ(out, "abcde");
    hub_auth_copy_name(out, sizeof(out), "a\xFF\x01z");
    CHECK_STR_EQ(out, "a??z");
    hub_auth_copy_name(out, sizeof(out), "\xC0\x80"); /* Overlong */
    CHECK_STR_EQ(out, "??");
    hub_auth_copy_name(out, sizeof(out), "\xED\xA0\x80"); /* A surrogate */
    CHECK_STR_EQ(out, "???");
    hub_auth_copy_name(out, sizeof(out), "\xF0\x9F\x8E\xB5!"); /* Four bytes, valid */
    CHECK_STR_EQ(out, "\xF0\x9F\x8E\xB5!");
    hub_auth_copy_name(out, sizeof(out), "\xE6\x97"); /* Cut short at the end */
    CHECK_STR_EQ(out, "??");
    hub_auth_copy_name(out, sizeof(out), NULL);
    CHECK_STR_EQ(out, "");

    /* A name saved cut by older firmware is mended as it is loaded. */
    hub_auth_store_t saved = {.next_cid = 2};
    saved.clients[0].used = true;
    saved.clients[0].cid = 1;
    memset(saved.clients[0].name, 'a', sizeof(saved.clients[0].name));
    memcpy(&saved.clients[0].name[AUTH_CLIENT_NAME_BYTES - 3], "\xE6\x97\xA5", 3);
    start();
    const hub_auth_io_t io = auth.io;
    hub_auth_init(&auth, &io, &saved);
    size_t count = 0;
    const hub_auth_client_t *clients = hub_auth_clients(&auth, &count);
    CHECK(strlen(clients[0].name) == AUTH_CLIENT_NAME_BYTES - 1);
    CHECK(strcmp(&clients[0].name[AUTH_CLIENT_NAME_BYTES - 3], "??") == 0); /* The cut character */
}

static void an_unreadable_pin_accepts_nothing_until_set(void)
{
    start();
    hub_auth_mark_unreadable(&auth);
    CHECK(hub_auth_pin_enabled(&auth) && hub_auth_unreadable(&auth));
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t cid = 0;
    for (int i = 0; i < 2 * AUTH_MAX_FAILURES_PER_ADDRESS; i++) {
        CHECK(hub_auth_check_pin(&auth, HOME, "000000") == HUB_AUTH_UNAVAILABLE);
        CHECK(hub_auth_pair(&auth, HOME, "000000", "x", token, &cid, NULL) == HUB_AUTH_UNAVAILABLE);
        CHECK(hub_auth_token(&auth, HOME, "wrong", &cid) == HUB_AUTH_UNAVAILABLE);
    }
    CHECK(hub_auth_issue(&auth, "x", token, &cid, NULL) == HUB_AUTH_UNAVAILABLE);
    CHECK(saves == 0);

    CHECK(hub_auth_set_pin(&auth, "482913") == HUB_AUTH_OK);
    CHECK(!hub_auth_unreadable(&auth));
    /* None of the refusals counted against the address. */
    CHECK(hub_auth_pair(&auth, HOME, "482913", "x", token, &cid, NULL) == HUB_AUTH_OK);
}

static void a_full_list_says_whom_it_forgot(void)
{
    start();
    hub_auth_set_pin(&auth, "482913");
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t first = 0;
    uint16_t cid = 0;
    uint16_t replaced = 99;
    hub_auth_issue(&auth, "Oldest", token, &first, &replaced);
    CHECK(replaced == 0);
    for (int i = 1; i < AUTH_MAX_PAIRED_CLIENTS; i++) {
        clock_unix += 10;
        hub_auth_issue(&auth, "Newer", token, &cid, &replaced);
        CHECK(replaced == 0);
    }
    clock_unix += 10;
    hub_auth_issue(&auth, "Newest", token, &cid, &replaced);
    CHECK(replaced == first);
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
    RUN(a_failed_hash_sets_no_pin);
    RUN(a_failed_hash_accepts_nothing_and_counts_nothing);
    RUN(a_failed_random_source_issues_no_token);
    RUN(lockouts_end_however_long_nobody_tries);
    RUN(a_lockout_is_never_given_up_for_room);
    RUN(names_are_cut_and_cleaned_into_utf8);
    RUN(an_unreadable_pin_accepts_nothing_until_set);
    RUN(a_full_list_says_whom_it_forgot);
}
