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
#include "test_frames.h"

#define BAUD 1000000

static fake_dspi_t fake;
static dspi_link_t link;
static uint8_t buffer[DSPI_PARSER_BUFFER_BYTES(LINK_MAX_PAYLOAD_BYTES)];

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

/* SYS.PLATFORM (0x0120). */
static const dspi_request_t GET_PLATFORM = {
    .is_get = true, .request = 0x20, .index = 0x0100, .length = 7};

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
    const dspi_request_t set_all = {.is_get = false,
                                    .request = 0x40, /* STORE.ALL_PARAMS, 0x0240 */
                                    .index = 0x0200,
                                    .length = sizeof(blob),
                                    .payload = blob};
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
        .is_get = false, .request = 0x50, .index = 0x0500, .length = 4, .payload = volume};
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
        .is_get = false, .request = 0x50, .index = 0x0500, .length = 4, .payload = volume};
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
    /* MEAS.RTA_STOP (0x0929): a zero-length SET. */
    const dspi_request_t set_mute = {
        .is_get = false, .request = 0x29, .index = 0x0900, .length = 0};
    dspi_reply_t reply = dspi_link_transact(&link, &set_mute, false);
    CHECK(reply.kind == DSPI_REPLY_CORRUPT);
    CHECK(dspi_link_counters(&link)->crc_errors == 1);
    CHECK(dspi_link_counters(&link)->timeouts == 0);

    fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_ANSWER_CORRUPT});
    reply = dspi_link_transact(&link, &GET_PLATFORM, true);
    CHECK(reply.kind == DSPI_REPLY_ANSWERED);
    CHECK(reply.attempts == 2);
}

/* SYS.SERIAL (0x0121). */
static const dspi_request_t GET_SERIAL = {
    .is_get = true, .request = 0x21, .index = 0x0100, .length = 16};

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

/* MEAS.RTA_STOP (0x0929): a zero-length SET. */
static const dspi_request_t SET_MUTE = {
    .is_get = false, .request = 0x29, .index = 0x0900, .length = 0};

static void repeated_timeouts_restart_the_uart(void)
{
    start(BAUD, BAUD);
    for (int i = 0; i < DSPI_UART_UNANSWERED_BEFORE_REINSTALL; i++) {
        fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_STAY_SILENT});
        CHECK(dspi_link_transact(&link, &SET_MUTE, false).kind == DSPI_REPLY_TIMEOUT);
    }
    CHECK(fake.restarts == 1);
    CHECK(dspi_link_failures_in_a_row(&link) == DSPI_UART_UNANSWERED_BEFORE_REINSTALL);

    /* One good answer clears the count. */
    CHECK(dspi_link_transact(&link, &GET_PLATFORM, true).kind == DSPI_REPLY_ANSWERED);
    CHECK(dspi_link_failures_in_a_row(&link) == 0);
}

/* A receiver stuck on noise sees damaged frames rather than silence; that
 * must still lead to the restart that clears it (bridge spec 7.4). */
static void noise_counts_towards_the_restart(void)
{
    start(BAUD, BAUD);
    /* Noise, then something shaped like a SET response with a wrong CRC. */
    const uint8_t noise[] = {0x13, 0xFF, 0xA5, 0x81, 0x07, 0x00, 0x00, 0x12, 0x34, 0x5A};
    for (int i = 0; i < DSPI_UART_UNANSWERED_BEFORE_REINSTALL; i++) {
        fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_STAY_SILENT});
        fake_dspi_send_raw(&fake, noise, sizeof(noise), 60); /* After any quarantine */
        CHECK(dspi_link_transact(&link, &SET_MUTE, false).kind == DSPI_REPLY_CORRUPT);
    }
    CHECK(fake.restarts == 1);
    CHECK(dspi_link_counters(&link)->restarts == 1);
    CHECK(dspi_link_counters(&link)->crc_errors == DSPI_UART_UNANSWERED_BEFORE_REINSTALL);
    CHECK(dspi_link_counters(&link)->timeouts == 0);
}

/* A damaged notification before silence is not the answer: the client is
 * told TIMEOUT, not CRC_ERROR. */
static void a_damaged_notification_is_not_taken_for_the_answer(void)
{
    start(BAUD, BAUD);
    const uint8_t damaged[] = {0xA5, 0x40, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00};
    fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_STAY_SILENT});
    fake_dspi_send_raw(&fake, damaged, sizeof(damaged), 5);
    CHECK(dspi_link_transact(&link, &SET_MUTE, false).kind == DSPI_REPLY_TIMEOUT);
    CHECK(dspi_link_counters(&link)->crc_errors == 1);
    CHECK(dspi_link_counters(&link)->timeouts == 1);
    CHECK(notified_count == 0);
}

/* A restart that fails is tried again on the next unanswered attempt
 * instead of waiting for another full run of failures. */
static void a_failed_restart_is_tried_again(void)
{
    start(BAUD, BAUD);
    fake.restart_fails = true;
    for (int i = 0; i < DSPI_UART_UNANSWERED_BEFORE_REINSTALL + 1; i++) {
        fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_STAY_SILENT});
        CHECK(dspi_link_transact(&link, &SET_MUTE, false).kind == DSPI_REPLY_TIMEOUT);
    }
    CHECK(fake.restarts == 2);

    fake.restart_fails = false;
    fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_STAY_SILENT});
    CHECK(dspi_link_transact(&link, &SET_MUTE, false).kind == DSPI_REPLY_TIMEOUT);
    CHECK(fake.restarts == 3);
    /* Succeeded: the count starts again from zero. */
    fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_STAY_SILENT});
    CHECK(dspi_link_transact(&link, &SET_MUTE, false).kind == DSPI_REPLY_TIMEOUT);
    CHECK(fake.restarts == 3);
}

static const char SERIAL[] = "E46058388B1A2E2C"; /* The fake's */

/* The probe's answer, for the tests that look for the device. */
static dspi_probe_t probe;

static void the_heartbeat_reads_the_serial(void)
{
    start(BAUD, BAUD);
    CHECK(dspi_link_heartbeat(&link, SERIAL) == DSPI_HEARTBEAT_SAME_DEVICE);
    CHECK(fake.record_count == 1);
    CHECK(fake.records[0].is_get && fake.records[0].request == 0x21);
    CHECK(fake.records[0].index == 0x0100);
    CHECK(fake.records[0].length == 16);

    /* Silence twice: a heartbeat is a safe read, so it gets its retry. */
    fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_STAY_SILENT});
    fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_STAY_SILENT});
    CHECK(dspi_link_heartbeat(&link, SERIAL) == DSPI_HEARTBEAT_NO_ANSWER);
    CHECK(fake.record_count == 3);

    /* A busy DSPi is still there, and nothing says it changed: each of the
     * three busy statuses. */
    const uint8_t busy[] = {DSPI_STATUS_BUSY, DSPI_STATUS_BULK_LOCKED, DSPI_STATUS_VC_ERR_BUSY};
    for (size_t i = 0; i < sizeof(busy); i++) {
        fake_dspi_script(&fake,
                         (fake_answer_t){.kind = FAKE_ANSWER_WITH_STATUS, .status = busy[i]});
        CHECK(dspi_link_heartbeat(&link, SERIAL) == DSPI_HEARTBEAT_SAME_DEVICE);
    }
}

/* A board that refuses the serial read does not know it: older firmware
 * swapped in answers a paged read with 0x02. It must not stay attached,
 * so it counts as another device and the next probe classifies it. */
/* A frame the DSPi rejected twice is line noise, not another board. */
static void a_damaged_heartbeat_is_no_answer(void)
{
    start(BAUD, BAUD);
    for (int i = 0; i < 2; i++) {
        fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_ANSWER_WITH_STATUS,
                                                .status = DSPI_STATUS_CRC_ERROR});
    }
    CHECK(dspi_link_heartbeat(&link, SERIAL) == DSPI_HEARTBEAT_NO_ANSWER);
}

static void a_refused_heartbeat_means_another_device(void)
{
    start(BAUD, BAUD);
    const uint8_t refusals[] = {DSPI_STATUS_ERROR, DSPI_STATUS_VC_ERR_UNKNOWN,
                                DSPI_STATUS_VC_ERR_ADDRESS};
    for (size_t i = 0; i < sizeof(refusals); i++) {
        fake_dspi_script(&fake,
                         (fake_answer_t){.kind = FAKE_ANSWER_WITH_STATUS, .status = refusals[i]});
        CHECK(dspi_link_heartbeat(&link, SERIAL) == DSPI_HEARTBEAT_OTHER_DEVICE);
    }
}

/* A DSPi swapped between two heartbeats never goes silent long enough to be
 * lost; its serial gives it away. */
static void the_heartbeat_spots_a_swapped_device(void)
{
    start(BAUD, BAUD);
    const uint8_t other[16] = "F00D58388B1A2E2C";
    fake_dspi_can(&fake, 0x0121, other, sizeof(other));
    CHECK(dspi_link_heartbeat(&link, SERIAL) == DSPI_HEARTBEAT_OTHER_DEVICE);

    /* A shorter serial that matches the start of the old one differs too. */
    const uint8_t prefix[16] = "E460";
    fake_dspi_can(&fake, 0x0121, prefix, sizeof(prefix));
    CHECK(dspi_link_heartbeat(&link, SERIAL) == DSPI_HEARTBEAT_OTHER_DEVICE);
}

static const uint8_t ONE[] = {0x01};
static const dspi_request_t SET_ONE = {.is_get = false,
                                       .request = 0x10,
                                       .index = 0x0800,
                                       .length = 1,
                                       .payload = ONE}; /* FX.CROSSFEED_ENABLE */

/* Noise that looks like the start of a frame claiming 64 bytes arrives
 * while idle. It must not swallow the next answer (audit UART-1). */
static void a_false_start_while_idle_does_not_swallow_the_answer(void)
{
    const uint8_t types[] = {DSPI_FRAME_TYPE_SET_RESPONSE, DSPI_FRAME_TYPE_GET_RESPONSE,
                             DSPI_FRAME_TYPE_NOTIFICATION};
    for (size_t i = 0; i < sizeof(types); i++) {
        start(BAUD, BAUD);
        const uint8_t glitch[] = {0xA5, types[i], 0x00, 0x40, 0x00};
        fake_dspi_send_raw(&fake, glitch, sizeof(glitch), 0);
        dspi_link_listen(&link, 10);

        const dspi_reply_t reply = dspi_link_transact(&link, &SET_ONE, false);
        CHECK(reply.kind == DSPI_REPLY_ANSWERED);
        CHECK(reply.attempts == 1);
        CHECK(fake.record_count == 1);
    }

    /* Hours later, too: the stale partial frame is dropped at once. */
    start(BAUD, BAUD);
    const uint8_t glitch[] = {0xA5, 0x40, 0x00, 0x40, 0x00};
    fake_dspi_send_raw(&fake, glitch, sizeof(glitch), 0);
    dspi_link_listen(&link, 10);
    fake.now_ms += 3600u * 1000u;
    const uint32_t started = fake.now_ms;
    CHECK(dspi_link_transact(&link, &SET_ONE, false).kind == DSPI_REPLY_ANSWERED);
    CHECK(fake.now_ms - started < 10);
}

/* A notification that lost a byte on the way takes the next frame's sync
 * byte as its CRC. The answer right behind it must still be found. */
static void a_notification_missing_a_byte_does_not_swallow_the_answer(void)
{
    start(BAUD, BAUD);
    const uint8_t packet[] = {0x02, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    uint8_t frame[32];
    size_t length =
        test_build_device_frame(DSPI_FRAME_TYPE_NOTIFICATION, 0, packet, sizeof(packet), frame);
    memmove(&frame[7], &frame[8], length - 8); /* One payload byte lost */
    length--;
    fake_dspi_send_raw(&fake, frame, length, 2);
    fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_ANSWER_LATE, .delay_ms = 1});

    const dspi_reply_t reply = dspi_link_transact(&link, &SET_ONE, false);
    CHECK(reply.kind == DSPI_REPLY_ANSWERED);
    CHECK(reply.status == DSPI_STATUS_OK);
    CHECK(notified_count == 0);
    CHECK(dspi_link_counters(&link)->crc_errors >= 1);
}

/* A whole notification and the answer hidden inside a false frame's
 * payload are both found once the false frame fails its CRC. */
static void frames_inside_a_false_frame_are_found(void)
{
    start(BAUD, BAUD);
    uint8_t wire[64] = {0xA5, 0x40, 0x00, 0x14, 0x00}; /* Claims 20 bytes */
    size_t length = 5;
    const uint8_t packet[] = {0x02, 0x00, 0x00, 0x07};
    length += test_build_device_frame(DSPI_FRAME_TYPE_NOTIFICATION, 0, packet, sizeof(packet),
                                      &wire[length]);
    fake_dspi_send_raw(&fake, wire, length, 2);
    fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_ANSWER_LATE, .delay_ms = 3});

    const dspi_reply_t reply = dspi_link_transact(&link, &GET_PLATFORM, false);
    CHECK(reply.kind == DSPI_REPLY_ANSWERED);
    CHECK(reply.length == 7);
    CHECK(reply.payload[0] == 0x01 && reply.payload[5] == 0x07);
    CHECK(notified_count == 1);
    CHECK(notified_length == sizeof(packet) && notified[3] == 0x07);
}

static unsigned slow_handler_calls;

/* A notification handler that holds the uart task for a second, as a hub
 * whose queue is full can (audit UART-2). */
static void slow_notification_handler(void *context, const uint8_t *packet, size_t length)
{
    (void)context;
    (void)packet;
    (void)length;
    slow_handler_calls++;
    fake.now_ms += 1000;
}

/* An answer that arrived in time but waited in the driver while a handler
 * held the uart task is still the answer. */
static void an_answer_held_up_by_a_slow_handler_still_counts(void)
{
    fake_dspi_init(&fake, BAUD);
    const dspi_port_t port = fake_dspi_port(&fake);
    dspi_link_init(&link, &port, BAUD, buffer, sizeof(buffer), slow_notification_handler, NULL);
    slow_handler_calls = 0;
    const uint8_t packet[] = {0x02, 0x00, 0x00, 0x01};
    fake_dspi_notify(&fake, packet, sizeof(packet), 3);
    fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_ANSWER_LATE, .delay_ms = 3});

    const dspi_reply_t reply = dspi_link_transact(&link, &SET_ONE, false);
    CHECK(slow_handler_calls == 1);
    CHECK(reply.kind == DSPI_REPLY_ANSWERED);
    CHECK(dspi_link_counters(&link)->timeouts == 0);
}

/* Probing for a missing DSPi changes the rate every attempt, which flushes
 * the receiver, so it never reinstalls the driver (audit UART-4). */
static void looking_for_a_missing_device_never_restarts_the_uart(void)
{
    start(BAUD, BAUD);
    fake.device_baud = 9600; /* Not a candidate: nothing will answer */
    const uint32_t candidates[] = DSPI_UART_BAUD_CANDIDATES;
    for (int pass = 0; pass < 4; pass++) {
        CHECK(!dspi_link_find_baud(&link, BAUD, candidates,
                                   sizeof(candidates) / sizeof(candidates[0]), &probe));
        CHECK(!dspi_link_find_baud(&link, BAUD, candidates, 0, &probe));
    }
    CHECK(fake.restarts == 0);
    CHECK(dspi_link_counters(&link)->restarts == 0);

    /* Once found, unanswered requests restart it as before. */
    fake.device_baud = BAUD;
    CHECK(dspi_link_find_baud(&link, BAUD, candidates, 0, &probe));
    for (int i = 0; i < DSPI_UART_UNANSWERED_BEFORE_REINSTALL; i++) {
        fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_STAY_SILENT});
        CHECK(dspi_link_transact(&link, &SET_MUTE, false).kind == DSPI_REPLY_TIMEOUT);
    }
    CHECK(fake.restarts == 1);
}

static void the_baud_rate_is_found(void)
{
    start(921600, 115200);
    const uint32_t candidates[] = DSPI_UART_BAUD_CANDIDATES;
    const bool found = dspi_link_find_baud(&link, 115200, candidates,
                                           sizeof(candidates) / sizeof(candidates[0]), &probe);
    CHECK(found);
    CHECK(dspi_link_baud(&link) == 921600);
    CHECK(fake.host_baud == 921600);
}

static void a_missing_device_leaves_the_first_rate(void)
{
    start(BAUD, BAUD);
    fake.device_baud = 9600; /* Not a candidate: nothing will answer */
    const uint32_t candidates[] = DSPI_UART_BAUD_CANDIDATES;
    CHECK(!dspi_link_find_baud(&link, BAUD, candidates, sizeof(candidates) / sizeof(candidates[0]),
                               &probe));
    CHECK(dspi_link_baud(&link) == BAUD);
}

/* The probe is SYS.PROTOCOL_INFO: command 0x10 on page 0x01, 16 bytes. */
static void the_probe_is_sys_protocol_info(void)
{
    start(BAUD, BAUD);
    fake_dspi_set_paged(&fake, true);
    CHECK(dspi_link_find_baud(&link, BAUD, NULL, 0, &probe));
    CHECK(fake.record_count == 1);
    CHECK(fake.records[0].is_get && fake.records[0].request == 0x10);
    CHECK(fake.records[0].index == 0x0100);
    CHECK(fake.records[0].length == 16);
    CHECK(probe.firmware == DSPI_FIRMWARE_PAGED);
    CHECK(probe.protocol_major == 1 && probe.protocol_minor == 0);
}

/* Firmware 1.1.6 refuses the probe with 0x02, which still proves the rate.
 * The only other request it is sent is its own platform read. */
static void older_firmware_is_too_old(void)
{
    start(BAUD, BAUD);
    fake_dspi_set_paged(&fake, false);
    CHECK(dspi_link_find_baud(&link, BAUD, NULL, 0, &probe));
    CHECK(probe.firmware == DSPI_FIRMWARE_TOO_OLD);

    uint8_t platform[DSPI_PLATFORM_BYTES];
    CHECK(dspi_link_read_legacy_platform(&link, platform) == DSPI_PLATFORM_BYTES);
    CHECK(fake.records[1].request == 0x7F && fake.records[1].index == 0);
    char version[16];
    dspi_platform_version(platform, DSPI_PLATFORM_BYTES, version, sizeof(version));
    CHECK(strcmp(version, "1.1.7") == 0); /* The fake's platform bytes */
}

/* A probe answered OK with something that is not protocol 1 is firmware
 * too new for this bridge. */
static void an_unknown_protocol_is_too_new(void)
{
    start(BAUD, BAUD);
    fake_dspi_set_paged(&fake, true);
    const uint8_t major_two[16] = {'D', 'S', 'P', 'i', 0x02, 0x00};
    fake_dspi_can(&fake, 0x0110, major_two, sizeof(major_two));
    CHECK(dspi_link_find_baud(&link, BAUD, NULL, 0, &probe));
    CHECK(probe.firmware == DSPI_FIRMWARE_TOO_NEW);

    const uint8_t not_dspi[16] = {'X', 'S', 'P', 'i', 0x01, 0x00};
    fake_dspi_can(&fake, 0x0110, not_dspi, sizeof(not_dspi));
    CHECK(dspi_link_find_baud(&link, BAUD, NULL, 0, &probe));
    CHECK(probe.firmware == DSPI_FIRMWARE_TOO_NEW);
}

/* A busy answer proves the rate but says nothing of the firmware yet. */
static void a_busy_probe_is_not_ready(void)
{
    start(BAUD, BAUD);
    fake_dspi_set_paged(&fake, true);
    fake_dspi_script(&fake,
                     (fake_answer_t){.kind = FAKE_ANSWER_WITH_STATUS, .status = DSPI_STATUS_BUSY});
    CHECK(dspi_link_find_baud(&link, BAUD, NULL, 0, &probe));
    CHECK(probe.firmware == DSPI_FIRMWARE_NOT_READY);
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
    fake_dspi_can(&fake, 0x0121, padded, sizeof(padded));
    dspi_identity_t identity;
    CHECK(dspi_link_read_identity(&link, &identity));
    CHECK(strcmp(identity.serial, "ABC") == 0);
}

/* How many times the fake was asked SYS.DEVICE_NAME (0x0124). */
static unsigned name_reads(void)
{
    unsigned count = 0;
    for (size_t i = 0; i < fake.record_count; i++) {
        count += fake.records[i].request == 0x24 && fake.records[i].index == 0x0100 ? 1 : 0;
    }
    return count;
}

static void the_identity_uses_the_paged_commands(void)
{
    start(BAUD, BAUD);
    dspi_identity_t identity;
    CHECK(dspi_link_read_identity(&link, &identity));
    static const uint16_t expected[] = {0x0121, 0x0120, 0x0B12, 0x0B10, 0x0124};
    CHECK(fake.record_count == sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        CHECK(fake.records[i].is_get);
        CHECK((uint16_t)((fake.records[i].index & 0xFF00) | fake.records[i].request) ==
              expected[i]);
    }
    CHECK(identity.uart_live && identity.has_uart_config && identity.notify_enabled);
    CHECK(identity.platform_length == 7);
}

static void the_board_name_is_read(void)
{
    start(BAUD, BAUD);
    uint8_t name[32] = "Rack left";
    fake_dspi_can(&fake, 0x0124, name, sizeof(name));
    dspi_identity_t identity;
    CHECK(dspi_link_read_identity(&link, &identity));
    CHECK(strcmp(identity.board_name, "Rack left") == 0);
    CHECK(name_reads() == 1);

    /* Unnamed is all zeros. */
    memset(name, 0, sizeof(name));
    fake_dspi_can(&fake, 0x0124, name, sizeof(name));
    CHECK(dspi_link_read_identity(&link, &identity));
    CHECK(identity.board_name[0] == '\0');

    /* A name without its NUL is cut to 31 characters. */
    memset(name, 'A', sizeof(name));
    fake_dspi_can(&fake, 0x0124, name, sizeof(name));
    CHECK(dspi_link_read_identity(&link, &identity));
    CHECK(strlen(identity.board_name) == 31);
}

/* Every 1.1.7 build answers the name in full. Without it the board cannot
 * be shown or renamed, so the identity is unread and the next probe tries
 * again, rather than the board being attached as one that keeps no name. */
static void a_board_whose_name_cannot_be_read_is_not_identified(void)
{
    start(BAUD, BAUD);
    for (int i = 0; i < 4; i++) { /* Serial, platform, interface status, UART config */
        fake_dspi_script(&fake, (fake_answer_t){.kind = FAKE_ANSWER_NORMALLY});
    }
    fake_dspi_script(
        &fake, (fake_answer_t){.kind = FAKE_ANSWER_WITH_STATUS, .status = DSPI_STATUS_VC_ERR_BUSY});
    dspi_identity_t identity;
    CHECK(!dspi_link_read_identity(&link, &identity));

    start(BAUD, BAUD);
    fake_dspi_can(&fake, 0x0124, (const uint8_t[]){1}, 1); /* A short reply */
    CHECK(!dspi_link_read_identity(&link, &identity));
}

/* SYS.PLATFORM is always 7 bytes on paged firmware; anything else is not
 * a DSPi the bridge can describe. */
static void a_short_platform_is_not_identified(void)
{
    start(BAUD, BAUD);
    fake_dspi_can(&fake, 0x0120, (const uint8_t[]){1, 1, 0x17, 9}, 4);
    dspi_identity_t identity;
    CHECK(!dspi_link_read_identity(&link, &identity));
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
    RUN(noise_counts_towards_the_restart);
    RUN(a_damaged_notification_is_not_taken_for_the_answer);
    RUN(a_failed_restart_is_tried_again);
    RUN(the_heartbeat_reads_the_serial);
    RUN(the_heartbeat_spots_a_swapped_device);
    RUN(a_damaged_heartbeat_is_no_answer);
    RUN(a_refused_heartbeat_means_another_device);
    RUN(a_false_start_while_idle_does_not_swallow_the_answer);
    RUN(a_notification_missing_a_byte_does_not_swallow_the_answer);
    RUN(frames_inside_a_false_frame_are_found);
    RUN(an_answer_held_up_by_a_slow_handler_still_counts);
    RUN(looking_for_a_missing_device_never_restarts_the_uart);
    RUN(the_baud_rate_is_found);
    RUN(a_missing_device_leaves_the_first_rate);
    RUN(the_probe_is_sys_protocol_info);
    RUN(older_firmware_is_too_old);
    RUN(an_unknown_protocol_is_too_new);
    RUN(a_busy_probe_is_not_ready);
    RUN(the_identity_is_read);
    RUN(a_short_serial_stops_at_its_padding);
    RUN(the_identity_uses_the_paged_commands);
    RUN(the_board_name_is_read);
    RUN(a_board_whose_name_cannot_be_read_is_not_identified);
    RUN(a_short_platform_is_not_identified);
}
