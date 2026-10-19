/*
 * test_net.c - the Wi-Fi state machine (bridge spec 9.1) and the setup
 * network's DNS answers (bridge spec 9.2).
 */

#include <string.h>

#include "bridge_config.h"
#include "captive_dns.h"
#include "check.h"
#include "net_machine.h"

static net_machine_t machine;
static uint32_t now;

/* ---------------------------------------------------------------------------
 * The state machine
 * ------------------------------------------------------------------------- */

static void a_new_bridge_shows_its_setup_network(void)
{
    now = 1000;
    net_machine_init(&machine, false, now);
    CHECK(machine.state == NET_STATE_SETUP);
    CHECK(machine.setup_network_on && !machine.station_wanted);
}

static void a_saved_network_is_joined_directly(void)
{
    now = 1000;
    net_machine_init(&machine, true, now);
    CHECK(machine.state == NET_STATE_CONNECTING);
    CHECK(!machine.setup_network_on && machine.station_wanted);
    net_machine_connected(&machine, now);
    CHECK(machine.state == NET_STATE_ONLINE && !machine.setup_network_on);
}

static void setup_joins_then_the_setup_network_goes_off(void)
{
    now = 1000;
    net_machine_init(&machine, false, now);
    net_machine_setup_clients(&machine, 1, now);
    net_machine_join(&machine, now);
    CHECK(machine.state == NET_STATE_CONNECTING && machine.join == NET_JOIN_CONNECTING);
    CHECK(machine.setup_network_on); /* The page needs it to show the result */

    now += 4000;
    net_machine_connected(&machine, now);
    CHECK(machine.join == NET_JOIN_CONNECTED);
    CHECK(machine.setup_network_on);

    net_machine_finish(&machine, now);
    now += SETUP_NETWORK_FINISH_DELAY_MS - 1;
    net_machine_tick(&machine, now);
    CHECK(machine.setup_network_on);
    now += 1;
    net_machine_tick(&machine, now);
    CHECK(!machine.setup_network_on);
}

static void without_finish_the_setup_network_still_goes_off(void)
{
    now = 1000;
    net_machine_init(&machine, false, now);
    net_machine_setup_clients(&machine, 1, now);
    net_machine_join(&machine, now);
    net_machine_connected(&machine, now);
    now += SETUP_NETWORK_AUTO_OFF_MS;
    net_machine_tick(&machine, now);
    CHECK(!machine.setup_network_on);
}

static void a_wrong_password_fails_at_once(void)
{
    now = 1000;
    net_machine_init(&machine, false, now);
    net_machine_join(&machine, now);
    net_machine_disconnected(&machine, now + 3000, true, false);
    CHECK(machine.join == NET_JOIN_FAILED);
    CHECK(machine.join_failure == NET_JOIN_FAILURE_WRONG_PASSWORD);
    CHECK(machine.state == NET_STATE_SETUP && !machine.has_network);
}

static void a_missing_network_fails_after_the_timeout(void)
{
    now = 1000;
    net_machine_init(&machine, false, now);
    net_machine_join(&machine, now);
    net_machine_disconnected(&machine, now + 2000, false, true);
    net_machine_tick(&machine, now + WIFI_JOIN_TIMEOUT_MS - 1);
    CHECK(machine.join == NET_JOIN_CONNECTING);
    net_machine_tick(&machine, now + WIFI_JOIN_TIMEOUT_MS);
    CHECK(machine.join_failure == NET_JOIN_FAILURE_NOT_FOUND);
}

static void a_failed_change_returns_to_the_saved_network(void)
{
    now = 1000;
    net_machine_init(&machine, true, now);
    net_machine_connected(&machine, now);
    net_machine_join(&machine, now); /* hub.wifi.join to another network */
    net_machine_tick(&machine, now + WIFI_JOIN_TIMEOUT_MS);
    CHECK(machine.join_failure == NET_JOIN_FAILURE_TIMED_OUT);
    CHECK(machine.restore_saved && machine.has_network);
    CHECK(machine.state == NET_STATE_CONNECTING);
}

static void a_lost_network_brings_back_the_setup_network(void)
{
    now = 1000;
    net_machine_init(&machine, true, now);
    net_machine_connected(&machine, now);
    net_machine_disconnected(&machine, now + 10, false, true);
    CHECK(machine.state == NET_STATE_CONNECTING && !machine.setup_network_on);

    net_machine_tick(&machine, now + 10 + WIFI_FALLBACK_AFTER_MS);
    CHECK(machine.state == NET_STATE_FALLBACK);
    CHECK(machine.setup_network_on && machine.station_wanted);

    /* The network returns; nobody is on the setup network, so it goes off
     * after the idle time. */
    now += 10 + WIFI_FALLBACK_AFTER_MS + 5000;
    net_machine_connected(&machine, now);
    CHECK(machine.state == NET_STATE_ONLINE && machine.setup_network_on);
    net_machine_tick(&machine, now + SETUP_NETWORK_IDLE_OFF_MS);
    CHECK(!machine.setup_network_on);
}

static void a_phone_on_the_setup_network_keeps_it_up(void)
{
    now = 1000;
    net_machine_init(&machine, true, now);
    net_machine_tick(&machine, now + WIFI_FALLBACK_AFTER_MS);
    net_machine_setup_clients(&machine, 1, now + WIFI_FALLBACK_AFTER_MS + 100);
    now += WIFI_FALLBACK_AFTER_MS + 200;
    net_machine_connected(&machine, now);
    net_machine_tick(&machine, now + SETUP_NETWORK_IDLE_OFF_MS);
    CHECK(machine.setup_network_on); /* Still in use */
    net_machine_tick(&machine, now + SETUP_NETWORK_AUTO_OFF_MS);
    CHECK(!machine.setup_network_on); /* But not for ever */
}

static void forgetting_returns_to_setup(void)
{
    now = 1000;
    net_machine_init(&machine, true, now);
    net_machine_connected(&machine, now);
    net_machine_forget(&machine, now);
    CHECK(machine.state == NET_STATE_SETUP && machine.setup_network_on);
    CHECK(!machine.station_wanted && !machine.has_network);
}

static void a_second_join_does_not_invent_a_saved_network(void)
{
    /* The setup page's Apply tapped twice on a new bridge: the first join
     * never worked, so there is nothing to return to when the second fails. */
    now = 1000;
    net_machine_init(&machine, false, now);
    net_machine_join(&machine, now);
    net_machine_join(&machine, now + 500);
    net_machine_tick(&machine, now + 500 + WIFI_JOIN_TIMEOUT_MS);
    CHECK(machine.join == NET_JOIN_FAILED);
    CHECK(!machine.restore_saved && !machine.has_network);
    CHECK(machine.state == NET_STATE_SETUP && !machine.station_wanted);
}

static void a_second_join_still_returns_to_the_saved_network(void)
{
    now = 1000;
    net_machine_init(&machine, true, now);
    net_machine_connected(&machine, now);
    net_machine_join(&machine, now);
    net_machine_join(&machine, now + 500);
    net_machine_tick(&machine, now + 500 + WIFI_JOIN_TIMEOUT_MS);
    CHECK(machine.restore_saved && machine.state == NET_STATE_CONNECTING);
}

static void a_network_that_worked_is_returned_to(void)
{
    /* Set up from scratch, then moved on with hub.wifi.join: the first
     * network has worked, so a failed second one returns to it. */
    now = 1000;
    net_machine_init(&machine, false, now);
    net_machine_join(&machine, now);
    net_machine_connected(&machine, now + 3000);
    net_machine_join(&machine, now + 10000);
    net_machine_tick(&machine, now + 10000 + WIFI_JOIN_TIMEOUT_MS);
    CHECK(machine.restore_saved && machine.state == NET_STATE_CONNECTING);
}

static void the_setup_network_forgets_its_phones_when_it_goes_off(void)
{
    /* The phone that pressed Finish is still on the setup network when it
     * goes off. The next time it comes back, nobody is on it, and the 60 s
     * rule must apply again. */
    now = 1000;
    net_machine_init(&machine, true, now);
    net_machine_tick(&machine, now + WIFI_FALLBACK_AFTER_MS);
    CHECK(machine.state == NET_STATE_FALLBACK);
    now += WIFI_FALLBACK_AFTER_MS + 1000;
    net_machine_setup_clients(&machine, 1, now);
    net_machine_connected(&machine, now);
    net_machine_finish(&machine, now);
    now += SETUP_NETWORK_FINISH_DELAY_MS;
    net_machine_tick(&machine, now);
    CHECK(!machine.setup_network_on && machine.setup_clients == 0);

    net_machine_disconnected(&machine, now, false, false);
    now += WIFI_FALLBACK_AFTER_MS;
    net_machine_tick(&machine, now);
    CHECK(machine.state == NET_STATE_FALLBACK && machine.setup_network_on);
    now += 1000;
    net_machine_connected(&machine, now);
    net_machine_tick(&machine, now + SETUP_NETWORK_IDLE_OFF_MS);
    CHECK(!machine.setup_network_on);
}

/* ---------------------------------------------------------------------------
 * DNS answers
 * ------------------------------------------------------------------------- */

static const uint8_t BRIDGE[4] = {192, 168, 4, 1};

/* A query for captive.apple.com of the given type. */
static size_t build_query(uint8_t *out, uint16_t type)
{
    static const uint8_t header[12] = {0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    static const uint8_t name[] = {7,   'c', 'a', 'p', 't', 'i', 'v', 'e', 5, 'a',
                                   'p', 'p', 'l', 'e', 3,   'c', 'o', 'm', 0};
    memcpy(out, header, sizeof(header));
    memcpy(out + 12, name, sizeof(name));
    size_t length = 12 + sizeof(name);
    out[length++] = (uint8_t)(type >> 8);
    out[length++] = (uint8_t)(type & 0xFF);
    out[length++] = 0;
    out[length++] = 1; /* IN */
    return length;
}

static void every_name_points_at_the_bridge(void)
{
    uint8_t query[64];
    uint8_t answer[128];
    const size_t query_length = build_query(query, 1);
    const size_t length = captive_dns_answer(query, query_length, BRIDGE, answer, sizeof(answer));

    CHECK(length == query_length + 16);
    CHECK(answer[0] == 0x12 && answer[1] == 0x34); /* Same id */
    CHECK((answer[2] & 0x80) != 0);                /* A response */
    CHECK(answer[7] == 1);                         /* One answer */
    CHECK(memcmp(&answer[length - 4], BRIDGE, 4) == 0);
}

static void other_types_get_an_empty_answer(void)
{
    uint8_t query[64];
    uint8_t answer[128];
    const size_t query_length = build_query(query, 28); /* AAAA */
    const size_t length = captive_dns_answer(query, query_length, BRIDGE, answer, sizeof(answer));
    CHECK(length == query_length);
    CHECK(answer[7] == 0);
}

static void malformed_queries_are_ignored(void)
{
    uint8_t query[64];
    uint8_t answer[128];
    const size_t query_length = build_query(query, 1);
    CHECK(captive_dns_answer(query, 10, BRIDGE, answer, sizeof(answer)) == 0);
    CHECK(captive_dns_answer(query, query_length - 3, BRIDGE, answer, sizeof(answer)) == 0);
    query[2] |= 0x80; /* A response, not a query */
    CHECK(captive_dns_answer(query, query_length, BRIDGE, answer, sizeof(answer)) == 0);
}

void test_net(void)
{
    RUN(a_new_bridge_shows_its_setup_network);
    RUN(a_saved_network_is_joined_directly);
    RUN(setup_joins_then_the_setup_network_goes_off);
    RUN(without_finish_the_setup_network_still_goes_off);
    RUN(a_wrong_password_fails_at_once);
    RUN(a_missing_network_fails_after_the_timeout);
    RUN(a_failed_change_returns_to_the_saved_network);
    RUN(a_lost_network_brings_back_the_setup_network);
    RUN(a_phone_on_the_setup_network_keeps_it_up);
    RUN(forgetting_returns_to_setup);
    RUN(a_second_join_does_not_invent_a_saved_network);
    RUN(a_second_join_still_returns_to_the_saved_network);
    RUN(a_network_that_worked_is_returned_to);
    RUN(the_setup_network_forgets_its_phones_when_it_goes_off);
    RUN(every_name_points_at_the_bridge);
    RUN(other_types_get_an_empty_answer);
    RUN(malformed_queries_are_ignored);
}
