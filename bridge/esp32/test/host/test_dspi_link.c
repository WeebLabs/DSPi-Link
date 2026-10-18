/*
 * test_dspi_link.c - the UART link engine (bridge spec 7.2 to 7.6) against
 * a simulated DSPi.
 */

#include <stdint.h>
#include <string.h>

#include "bridge_config.h"
#include "check.h"
#include "dspi_link.h"
#include "fake_dspi.h"

#define BAUD 1000000

static fake_dspi_t fake;
static dspi_link_t link;
static uint8_t buffer[LINK_MAX_PAYLOAD_BYTES];

/* The last notification the link relayed. */
static uint8_t notified[64];
static size_t notified_length;
static unsigned notified_count;

static void remember_notification(void *context, const uint8_t *packet, size_t length)
{
    (void)context;
    memcpy(notified, packet, length);
    notified_length = length;
    notified_count++;
}

/* A fresh fake DSPi and link at `device_baud`, with the link at `link_baud`. */
static void start(uint32_t device_baud, uint32_t link_baud)
{
    fake_dspi_init(&fake, device_baud);
    const dspi_port_t port = fake_dspi_port(&fake);
    dspi_link_init(&link, &port, link_baud, buffer, sizeof(buffer), remember_notification, NULL);
    fake.host_baud = link_baud;
    notified_length = 0;
    notified_count = 0;
}

static const dspi_request_t GET_PLATFORM = {.is_get = true, .request = 0x7F, .length = 7};

static void a_get_returns_its_payload(void)
{
    start(BAUD, BAUD);
    const dspi_reply_t reply = dspi_link_transact(&link, &GET_PLATFORM, true);

    CHECK(reply.kind == DSPI_REPLY_ANSWERED);
    CHECK(reply.status == DSPI_STATUS_OK);
    CHECK(reply.length == 7);
    CHECK(reply.payload[0] == 0x01 && reply.payload[5] == 0x07);
    CHECK(reply.attempts == 1);
    CHECK(dspi_link_failures_in_a_row(&link) == 0);
}

static void a_bulk_set_arrives_intact(void)
{
    start(BAUD, BAUD);
    static uint8_t blob[6136];
    for (size_t i = 0; i < sizeof(blob); i++) {
        blob[i] = (uint8_t)(i * 7);
    }
    const dspi_request_t set_all = {
        .is_get = false, .request = 0xA1, .length = sizeof(blob), .payload = blob};
    const dspi_reply_t reply = dspi_link_transact(&link, &set_all, false);

    CHECK(reply.kind == DSPI_REPLY_ANSWERED);
    CHECK(reply.status == DSPI_STATUS_OK);
    CHECK(reply.length == 0);
    CHECK(fake.last_payload_length == sizeof(blob));
    CHECK(memcmp(fake.last_payload, blob, sizeof(blob)) == 0);
}

static void notifications_are_relayed_while_waiting(void)
{
    start(BAUD, BAUD);
    const uint8_t packet[] = {0x02, 0x07, 0x01, 0x00, 0x2A};
    fake_dspi_notify(&fake, packet, sizeof(packet), 0); /* Arrives before the answer */
    const dspi_reply_t reply = dspi_link_transact(&link, &GET_PLATFORM, true);

    CHECK(reply.kind == DSPI_REPLY_ANSWERED);
    CHECK(reply.length == 7);
    CHECK(notified_count == 1);
    CHECK(notified_length == sizeof(packet));
    CHECK(memcmp(notified, packet, sizeof(packet)) == 0);
}

static void listening_relays_notifications(void)
{
    start(BAUD, BAUD);
    const uint8_t packet[] = {0x02, 0x08};
    fake_dspi_notify(&fake, packet, sizeof(packet), 3);
    dspi_link_listen(&link, 10);
    CHECK(notified_count == 1);
}

static void a_safe_get_is_retried_after_silence(void)
{
    start(BAUD, BAUD);
    fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_STAY_SILENT});
    const dspi_reply_t reply = dspi_link_transact(&link, &GET_PLATFORM, true);

    CHECK(reply.kind == DSPI_REPLY_ANSWERED);
    CHECK(reply.attempts == 2);
    CHECK(fake.record_count == 2);
    CHECK(dspi_link_counters(&link)->timeouts == 1);
    CHECK(dspi_link_counters(&link)->retries == 1);
}

static void an_unsafe_request_is_not_retried_after_silence(void)
{
    start(BAUD, BAUD);
    fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_STAY_SILENT});
    const uint8_t volume[] = {0x00, 0x00, 0xA0, 0xC1};
    const dspi_request_t set_volume = {
        .is_get = false, .request = 0xD2, .length = 4, .payload = volume};
    const uint32_t started = fake.now_ms;
    const dspi_reply_t reply = dspi_link_transact(&link, &set_volume, false);

    CHECK(reply.kind == DSPI_REPLY_TIMEOUT);
    CHECK(reply.attempts == 1);
    CHECK(fake.record_count == 1);
    CHECK(dspi_link_failures_in_a_row(&link) == 1);
    /* About 150 ms of margin plus the wire time; never seconds. */
    CHECK(fake.now_ms - started >= DSPI_UART_TIMEOUT_MARGIN_MS);
    CHECK(fake.now_ms - started < DSPI_UART_TIMEOUT_MARGIN_MS + 20);
}

static void any_request_is_resent_when_the_dspi_rejects_the_frame(void)
{
    start(BAUD, BAUD);
    fake_dspi_script(
        &fake, (fake_answer_t){.kind = FAKE_ANSWER_WITH_STATUS, .status = DSPI_STATUS_CRC_ERROR});
    const uint8_t volume[] = {0x00, 0x00, 0xA0, 0xC1};
    const dspi_request_t set_volume = {
        .is_get = false, .request = 0xD2, .length = 4, .payload = volume};
    const dspi_reply_t reply = dspi_link_transact(&link, &set_volume, false);

    CHECK(reply.kind == DSPI_REPLY_ANSWERED);
    CHECK(reply.status == DSPI_STATUS_OK);
    CHECK(reply.attempts == 2);
}

static void busy_is_passed_on_without_a_retry(void)
{
    start(BAUD, BAUD);
    fake_dspi_script(&fake,
                     (fake_answer_t){.kind = FAKE_ANSWER_WITH_STATUS, .status = DSPI_STATUS_BUSY});
    const dspi_reply_t reply = dspi_link_transact(&link, &GET_PLATFORM, true);

    CHECK(reply.kind == DSPI_REPLY_ANSWERED);
    CHECK(reply.status == DSPI_STATUS_BUSY);
    CHECK(reply.attempts == 1);
    CHECK(reply.length == 0);
}

static void a_corrupt_answer_is_reported_or_retried(void)
{
    start(BAUD, BAUD);
    fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_ANSWER_CORRUPT});
    const dspi_request_t set_mute = {.is_get = false, .request = 0xD4, .length = 0};
    dspi_reply_t reply = dspi_link_transact(&link, &set_mute, false);
    CHECK(reply.kind == DSPI_REPLY_CORRUPT);
    CHECK(dspi_link_counters(&link)->crc_errors == 1);
    CHECK(dspi_link_counters(&link)->timeouts == 0);

    fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_ANSWER_CORRUPT});
    reply = dspi_link_transact(&link, &GET_PLATFORM, true);
    CHECK(reply.kind == DSPI_REPLY_ANSWERED);
    CHECK(reply.attempts == 2);
}

static const dspi_request_t GET_SERIAL = {.is_get = true, .request = 0x7E, .length = 16};

/* After a timeout, the late answer arrives while the link is still
 * draining the line, and is dropped there. */
static void a_late_answer_during_quarantine_is_dropped(void)
{
    start(BAUD, BAUD);
    fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_ANSWER_LATE, .delay_ms = 160});
    CHECK(dspi_link_transact(&link, &GET_SERIAL, false).kind == DSPI_REPLY_TIMEOUT);

    const dspi_reply_t second = dspi_link_transact(&link, &GET_PLATFORM, false);
    CHECK(second.kind == DSPI_REPLY_ANSWERED);
    CHECK(second.length == 7);
    CHECK(second.payload[0] == 0x01 && second.payload[1] == 0x01);
}

/* The late answer arrives after the quarantine ended, just as the next
 * request is answered. It is longer than that request asked for, so it
 * cannot be its answer. */
static void a_late_answer_after_quarantine_is_recognised(void)
{
    start(BAUD, BAUD);
    fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_ANSWER_LATE, .delay_ms = 200});
    CHECK(dspi_link_transact(&link, &GET_SERIAL, false).kind == DSPI_REPLY_TIMEOUT);

    const dspi_reply_t second = dspi_link_transact(&link, &GET_PLATFORM, false);
    CHECK(second.kind == DSPI_REPLY_ANSWERED);
    CHECK(second.length == 7);
    CHECK(second.payload[0] == 0x01 && second.payload[1] == 0x01);
}

static void requests_keep_the_idle_gap(void)
{
    start(BAUD, BAUD);
    for (int i = 0; i < 5; i++) {
        dspi_link_transact(&link, &GET_PLATFORM, true);
    }
    CHECK(fake.record_count == 5);
    CHECK(fake.smallest_gap_ms >= DSPI_UART_IDLE_GAP_MS);
}

static void repeated_timeouts_restart_the_uart(void)
{
    start(BAUD, BAUD);
    const dspi_request_t set_mute = {.is_get = false, .request = 0xD4, .length = 0};
    for (int i = 0; i < DSPI_UART_TIMEOUTS_BEFORE_REINSTALL; i++) {
        fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_STAY_SILENT});
        dspi_link_transact(&link, &set_mute, false);
    }
    CHECK(fake.restarts == 1);
    CHECK(dspi_link_failures_in_a_row(&link) == DSPI_UART_TIMEOUTS_BEFORE_REINSTALL);

    /* One good answer clears the count. */
    CHECK(dspi_link_transact(&link, &GET_PLATFORM, true).kind == DSPI_REPLY_ANSWERED);
    CHECK(dspi_link_failures_in_a_row(&link) == 0);
}

static void the_baud_rate_is_found(void)
{
    start(921600, 115200);
    const uint32_t candidates[] = DSPI_UART_BAUD_CANDIDATES;
    const bool found =
        dspi_link_find_baud(&link, 115200, candidates, sizeof(candidates) / sizeof(candidates[0]));
    CHECK(found);
    CHECK(dspi_link_baud(&link) == 921600);
    CHECK(fake.host_baud == 921600);
}

static void a_missing_device_leaves_the_first_rate(void)
{
    start(BAUD, BAUD);
    fake.device_baud = 9600; /* Not a candidate: nothing will answer */
    const uint32_t candidates[] = DSPI_UART_BAUD_CANDIDATES;
    CHECK(
        !dspi_link_find_baud(&link, BAUD, candidates, sizeof(candidates) / sizeof(candidates[0])));
    CHECK(dspi_link_baud(&link) == BAUD);
}

static void the_identity_is_read(void)
{
    start(BAUD, BAUD);
    dspi_identity_t identity;
    CHECK(dspi_link_read_identity(&link, &identity));
    CHECK(strcmp(identity.serial, "E46058388B1A2E2C") == 0);
    CHECK(identity.platform_length == 7);
    CHECK(identity.platform[0] == 0x01);
    CHECK(identity.uart_live);
    CHECK(identity.has_uart_config);
    CHECK(identity.notify_enabled);
    CHECK(identity.configured_baud == 1000000);
}

static void a_short_serial_stops_at_its_padding(void)
{
    start(BAUD, BAUD);
    const uint8_t padded[16] = {'A', 'B', 'C', 0};
    fake_dspi_can(&fake, 0x7E, padded, sizeof(padded));
    dspi_identity_t identity;
    CHECK(dspi_link_read_identity(&link, &identity));
    CHECK(strcmp(identity.serial, "ABC") == 0);
}

void test_dspi_link(void)
{
    RUN(a_get_returns_its_payload);
    RUN(a_bulk_set_arrives_intact);
    RUN(notifications_are_relayed_while_waiting);
    RUN(listening_relays_notifications);
    RUN(a_safe_get_is_retried_after_silence);
    RUN(an_unsafe_request_is_not_retried_after_silence);
    RUN(any_request_is_resent_when_the_dspi_rejects_the_frame);
    RUN(busy_is_passed_on_without_a_retry);
    RUN(a_corrupt_answer_is_reported_or_retried);
    RUN(a_late_answer_during_quarantine_is_dropped);
    RUN(a_late_answer_after_quarantine_is_recognised);
    RUN(requests_keep_the_idle_gap);
    RUN(repeated_timeouts_restart_the_uart);
    RUN(the_baud_rate_is_found);
    RUN(a_missing_device_leaves_the_first_rate);
    RUN(the_identity_is_read);
    RUN(a_short_serial_stops_at_its_padding);
}
