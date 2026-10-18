/*
 * test_hub.c - the hub state machine (Link spec 5 to 8, bridge spec 8),
 * driven through scripted sessions, a recording I/O and a fake clock.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "check.h"
#include "fake_hub_io.h"
#include "hub.h"
#include "link_frames.h"

static fake_hub_io_t io_record;
static hub_t hub;

static const dspi_identity_t IDENTITY = {
    .serial = "E46058388B1A2E2C",
    .platform = {0x01, 0x01, 0x17, 0x09, 0x01, 0x07, 0x02},
    .platform_length = 7,
    .uart_live = true,
    .notify_enabled = true,
    .configured_baud = 1000000,
    .has_uart_config = true,
};

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static void start(void)
{
    const hub_io_t io = fake_hub_io_init(&io_record);
    const hub_config_t config = {
        .hub_id = "a664b936-5e26-4662-b2f5-8b18949721d6",
        .name = "DSPi Bridge 1B34",
        .version = "0.1.0",
        .max_sessions = 4,
    };
    hub_init(&hub, &io, &config);
}

static void forget_messages(void)
{
    io_record.message_count = 0;
}

static void forget_submissions(void)
{
    io_record.submission_count = 0;
}

static void send_json(int slot, const char *format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    hub_session_text(&hub, slot, text, strlen(text));
}

/* The last JSON message sent to `slot` whose "t" is `type`, parsed; the
 * caller deletes it. NULL if there is none. */
static cJSON *last_json(int slot, const char *type)
{
    for (size_t i = io_record.message_count; i > 0; i--) {
        const fake_message_t *message = &io_record.messages[i - 1];
        if (message->slot != slot || message->binary) {
            continue;
        }
        cJSON *json = cJSON_ParseWithLength((const char *)message->data, message->length);
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(json, "t");
        if (cJSON_IsString(t) && strcmp(t->valuestring, type) == 0) {
            return json;
        }
        cJSON_Delete(json);
    }
    return NULL;
}

/* The last binary frame of `type` sent to `slot`, or NULL. */
static const fake_message_t *last_frame(int slot, uint8_t type)
{
    for (size_t i = io_record.message_count; i > 0; i--) {
        const fake_message_t *message = &io_record.messages[i - 1];
        if (message->slot == slot && message->binary && message->data[0] == type) {
            return message;
        }
    }
    return NULL;
}

static int json_number(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) ? item->valueint : -1;
}

static const char *json_string(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : "";
}

/* Opens a session on `slot`, says hello and pairs, as Console does with
 * `auth: none`. Returns the session id. */
static int open_session(int slot)
{
    hub_session_opened(&hub, slot);
    send_json(slot,
              "{\"t\":\"hello\",\"proto\":{\"major\":1,\"minor\":1},"
              "\"client\":{\"name\":\"Test %d\"}}",
              slot);
    send_json(slot, "{\"t\":\"auth.pair\",\"id\":1,\"pin\":\"000000\",\"name\":\"Test\"}");
    cJSON *ok = last_json(slot, "ok");
    const int session_id = json_number(ok, "session");
    cJSON_Delete(ok);
    return session_id;
}

/* Brings the DSPi online and completes the background snapshot read the
 * hub starts, so tests begin from a quiet link. */
static void attach_device(void)
{
    hub_device_found(&hub, &IDENTITY, 1000000);
    /* bulk read, then status read */
    static uint8_t bulk[6136];
    bulk[0] = 32; /* wire version */
    bulk[2] = 17; /* channels */
    bulk[4] = 8;  /* inputs */
    const dspi_reply_t bulk_reply = {
        .kind = DSPI_REPLY_ANSWERED, .payload = bulk, .length = sizeof(bulk)};
    hub_reply(&hub, io_record.submissions[io_record.submission_count - 1].tag, &bulk_reply);
    static const uint8_t status[41] = {1, 2, 3};
    const dspi_reply_t status_reply = {
        .kind = DSPI_REPLY_ANSWERED, .payload = status, .length = sizeof(status)};
    hub_reply(&hub, io_record.submissions[io_record.submission_count - 1].tag, &status_reply);
    forget_submissions();
}

/* Sends a CMD request frame. */
static void send_cmd(int slot, uint16_t tag, bool is_get, uint8_t request, uint16_t length,
                     const uint8_t *payload)
{
    static uint8_t frame[LINK_CMD_REQUEST_HEADER_BYTES + LINK_MAX_PAYLOAD_BYTES];
    frame[0] = LINK_FRAME_CMD_REQUEST;
    frame[1] = 0;
    frame[2] = (uint8_t)(tag & 0xFF);
    frame[3] = (uint8_t)(tag >> 8);
    frame[4] = 0; /* handle */
    frame[5] = is_get ? 1 : 0;
    frame[6] = request;
    frame[7] = 0;
    frame[8] = 0;
    frame[9] = 0;
    frame[10] = 2;
    frame[11] = 0;
    frame[12] = (uint8_t)(length & 0xFF);
    frame[13] = (uint8_t)(length >> 8);
    const size_t payload_length = is_get ? 0 : length;
    if (payload_length > 0) {
        memcpy(&frame[14], payload, payload_length);
    }
    hub_session_binary(&hub, slot, frame, LINK_CMD_REQUEST_HEADER_BYTES + payload_length);
}

/* The status byte of the last CMD response to `slot`, or -1. */
static int last_status(int slot)
{
    const fake_message_t *response = last_frame(slot, LINK_FRAME_CMD_RESPONSE);
    return response != NULL ? response->data[4] : -1;
}

static void answer_last(uint8_t status, const uint8_t *payload, uint16_t length)
{
    const dspi_reply_t reply = {
        .kind = DSPI_REPLY_ANSWERED, .status = status, .payload = payload, .length = length};
    hub_reply(&hub, io_record.submissions[io_record.submission_count - 1].tag, &reply);
}

/* A v2 PARAM_CHANGED packet with the given seq and source byte. */
static void notify(uint8_t seq, uint8_t source)
{
    const uint8_t packet[16] = {0x02, 0x02, 0x00, seq, 0x5C, 0x12, 0x04, 0x00, source};
    hub_notification(&hub, packet, sizeof(packet));
}

/* ---------------------------------------------------------------------------
 * Sessions and hello
 * ------------------------------------------------------------------------- */

static void hello_describes_the_bridge(void)
{
    start();
    hub_session_opened(&hub, 0);
    send_json(0, "{\"t\":\"hello\",\"proto\":{\"major\":1,\"minor\":1}}");

    cJSON *hello = last_json(0, "hello");
    CHECK(hello != NULL);
    const cJSON *proto = cJSON_GetObjectItemCaseSensitive(hello, "proto");
    CHECK(json_number(proto, "major") == 1 && json_number(proto, "minor") == 2);
    const cJSON *hub_object = cJSON_GetObjectItemCaseSensitive(hello, "hub");
    CHECK(strcmp(json_string(hub_object, "kind"), "bridge") == 0);
    CHECK(strcmp(json_string(hub_object, "version"), "0.1.0") == 0);
    CHECK(strcmp(json_string(hello, "auth"), "none") == 0);
    const cJSON *limits = cJSON_GetObjectItemCaseSensitive(hello, "limits");
    CHECK(json_number(limits, "max_frame") == 16384);
    CHECK(json_number(limits, "poll_budget_bps") == 40000);
    cJSON_Delete(hello);
}

static void the_first_message_must_be_hello(void)
{
    start();
    hub_session_opened(&hub, 0);
    send_json(0, "{\"t\":\"device.list\",\"id\":1}");
    CHECK(io_record.close_code[0] == 4000);
}

static void pairing_without_auth_reports_the_session(void)
{
    start();
    const int first = open_session(0);
    const int second = open_session(1);
    CHECK(first > 0 && second > 0 && first != second);

    cJSON *ok = last_json(0, "ok");
    CHECK(strcmp(json_string(ok, "role"), "admin") == 0);
    cJSON_Delete(ok);
}

static void unknown_requests_get_unknown_type(void)
{
    start();
    open_session(0);
    send_json(0, "{\"t\":\"ping\",\"id\":9}"); /* The browser client's keepalive */
    cJSON *err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "code"), "unknown_type") == 0);
    CHECK(json_number(err, "id") == 9);
    cJSON_Delete(err);
}

static void a_full_bridge_has_no_room(void)
{
    start();
    for (int slot = 0; slot < 4; slot++) {
        CHECK(hub_has_room(&hub));
        hub_session_opened(&hub, slot);
    }
    CHECK(!hub_has_room(&hub));
    hub_session_closed(&hub, 2);
    CHECK(hub_has_room(&hub));
}

static void keepalive_pings_then_closes_a_silent_session(void)
{
    start();
    open_session(0);
    io_record.now_ms += LINK_PING_INTERVAL_MS;
    hub_tick(&hub);
    CHECK(io_record.pings[0] == 1);

    io_record.now_ms += LINK_PING_TIMEOUT_MS;
    hub_tick(&hub);
    CHECK(io_record.close_code[0] == 1001);
}

static void a_session_that_never_says_hello_is_closed(void)
{
    start();
    hub_session_opened(&hub, 0);
    io_record.now_ms += LINK_AUTH_TIMEOUT_MS;
    hub_tick(&hub);
    CHECK(io_record.close_code[0] == 4001);
}

/* ---------------------------------------------------------------------------
 * The device
 * ------------------------------------------------------------------------- */

static void a_found_device_is_announced_and_listed(void)
{
    start();
    open_session(0);
    hub_device_found(&hub, &IDENTITY, 1000000);

    cJSON *added = last_json(0, "device.added");
    const cJSON *device = cJSON_GetObjectItemCaseSensitive(added, "device");
    CHECK(strcmp(json_string(device, "serial"), "E46058388B1A2E2C") == 0);
    CHECK(strcmp(json_string(device, "name"), "DSPi 2E2C") == 0);
    CHECK(strcmp(json_string(device, "fw"), "1.1.7") == 0);
    CHECK(json_number(device, "fw_beta") == 2);
    CHECK(json_number(device, "outputs") == 9);
    CHECK(strcmp(json_string(device, "link"), "uart") == 0);
    CHECK(strcmp(json_string(device, "state"), "online") == 0);
    cJSON_Delete(added);

    /* The background read gives the device object its wire version. */
    CHECK(io_record.submission_count == 1);
    CHECK(io_record.submissions[0].request.request == 0xA0);
    static uint8_t bulk[6136] = {32, 1, 17, 9, 8};
    answer_last(0, bulk, sizeof(bulk));
    cJSON *changed = last_json(0, "device.changed");
    device = cJSON_GetObjectItemCaseSensitive(changed, "device");
    CHECK(json_number(device, "wire_version") == 32);
    CHECK(json_number(device, "inputs") == 8);
    cJSON_Delete(changed);
}

static void a_lost_device_goes_offline_then_away(void)
{
    start();
    open_session(0);
    attach_device();
    hub_device_lost(&hub);
    cJSON *changed = last_json(0, "device.changed");
    CHECK(strcmp(json_string(cJSON_GetObjectItemCaseSensitive(changed, "device"), "state"),
                 "offline") == 0);
    cJSON_Delete(changed);

    io_record.now_ms += LINK_OFFLINE_HANDLE_RETAIN_MS;
    hub_session_activity(&hub, 0); /* Keep the session alive past its keepalive limit */
    hub_tick(&hub);
    cJSON *removed = last_json(0, "device.removed");
    CHECK(removed != NULL);
    cJSON_Delete(removed);
}

static void a_returning_device_triggers_resync(void)
{
    start();
    open_session(0);
    attach_device();
    hub_device_lost(&hub);
    forget_messages();
    hub_device_found(&hub, &IDENTITY, 1000000);
    const fake_message_t *resync = last_frame(0, LINK_FRAME_RESYNC);
    CHECK(resync != NULL && resync->data[5] == LINK_RESYNC_REATTACHED);
}

static void devices_can_be_renamed(void)
{
    start();
    open_session(0);
    attach_device();
    send_json(0, "{\"t\":\"device.rename\",\"id\":4,\"handle\":0,\"name\":\"Living room\"}");
    CHECK(strcmp(io_record.saved_device_name, "Living room") == 0);
    cJSON *changed = last_json(0, "device.changed");
    CHECK(strcmp(json_string(cJSON_GetObjectItemCaseSensitive(changed, "device"), "name"),
                 "Living room") == 0);
    cJSON_Delete(changed);
}

/* ---------------------------------------------------------------------------
 * Commands (Link spec 8.2, bridge spec 8.3)
 * ------------------------------------------------------------------------- */

static void a_get_goes_to_the_dspi_and_back(void)
{
    start();
    open_session(0);
    attach_device();
    send_cmd(0, 77, true, 0x7F, 7, NULL);

    CHECK(io_record.submission_count == 1);
    CHECK(io_record.submissions[0].request.request == 0x7F);
    CHECK(io_record.submissions[0].may_retry); /* A read */

    const uint8_t platform[7] = {1, 1, 0x17, 9, 1, 7, 2};
    answer_last(0, platform, 7);
    const fake_message_t *response = last_frame(0, LINK_FRAME_CMD_RESPONSE);
    CHECK(response != NULL);
    CHECK(response->data[2] == 77 && response->data[4] == 0 && response->data[6] == 7);
    CHECK(memcmp(&response->data[8], platform, 7) == 0);
}

static void write_as_read_commands_are_never_retried(void)
{
    start();
    open_session(0);
    attach_device();
    send_cmd(0, 1, true, 0x0E, 1, NULL); /* REQ_RTA_CONTROL */
    CHECK(io_record.submission_count == 1 && !io_record.submissions[0].may_retry);
}

static void commands_without_a_device_are_refused(void)
{
    start();
    open_session(0);
    send_cmd(0, 3, true, 0x7F, 7, NULL);
    CHECK(last_status(0) == LINK_STATUS_NO_DEVICE);
    CHECK(io_record.submission_count == 0);
}

static void too_many_commands_are_rate_limited(void)
{
    start();
    open_session(0);
    attach_device();
    for (uint16_t tag = 1; tag <= LINK_MAX_INFLIGHT; tag++) {
        send_cmd(0, tag, true, 0x7F, 7, NULL);
    }
    CHECK(io_record.submission_count == LINK_MAX_INFLIGHT);
    send_cmd(0, 99, true, 0x7F, 7, NULL);
    CHECK(last_status(0) == LINK_STATUS_RATE_LIMITED);
}

static void long_sets_are_oversize_but_bulk_sets_go_through(void)
{
    start();
    open_session(0);
    attach_device();
    static uint8_t payload[6136];
    send_cmd(0, 5, false, 0x3E, 65, payload); /* Not bulk, over 64 bytes */
    CHECK(last_status(0) == DSPI_STATUS_OVERSIZE);
    CHECK(io_record.submission_count == 0);

    payload[100] = 0x5A;
    send_cmd(0, 6, false, 0xA1, sizeof(payload), payload); /* REQ_SET_ALL_PARAMS */
    CHECK(io_record.submission_count == 1);
    CHECK(io_record.submissions[0].request.length == sizeof(payload));
    CHECK(io_record.submissions[0].payload[100] == 0x5A);
}

static void failures_map_to_link_statuses(void)
{
    start();
    open_session(0);
    attach_device();
    send_cmd(0, 1, true, 0x7F, 7, NULL);
    hub_reply(&hub, io_record.submissions[0].tag, &(dspi_reply_t){.kind = DSPI_REPLY_TIMEOUT});
    CHECK(last_status(0) == LINK_STATUS_TIMEOUT);

    send_cmd(0, 2, true, 0x7F, 7, NULL);
    hub_reply(&hub, io_record.submissions[1].tag, &(dspi_reply_t){.kind = DSPI_REPLY_CORRUPT});
    CHECK(last_status(0) == DSPI_STATUS_CRC_ERROR);

    send_cmd(0, 3, true, 0x7F, 7, NULL);
    answer_last(DSPI_STATUS_BUSY, NULL, 0);
    CHECK(last_status(0) == DSPI_STATUS_BUSY);
}

static void a_reply_for_a_closed_session_goes_nowhere(void)
{
    start();
    open_session(0);
    attach_device();
    send_cmd(0, 1, true, 0x7F, 7, NULL);
    hub_session_closed(&hub, 0);
    open_session(0); /* The slot is reused by someone else */
    forget_messages();

    answer_last(0, (const uint8_t[]){1, 2, 3}, 3);
    CHECK(last_frame(0, LINK_FRAME_CMD_RESPONSE) == NULL);
}

/* ---------------------------------------------------------------------------
 * Notifications (Link spec 8.3)
 * ------------------------------------------------------------------------- */

static void notifications_carry_the_writers_session(void)
{
    start();
    const int writer = open_session(0);
    open_session(1);
    attach_device();
    send_cmd(0, 1, false, 0xD2, 4, (const uint8_t[]){0, 0, 0xA0, 0xC1});
    answer_last(0, NULL, 0);
    notify(1, 1 /* HOST_SET */);

    for (int slot = 0; slot < 2; slot++) {
        const fake_message_t *frame = last_frame(slot, LINK_FRAME_NOTIFY);
        CHECK(frame != NULL);
        CHECK((frame->data[6] | (frame->data[7] << 8)) == writer);
    }

    notify(2, 3 /* PRESET */);
    const fake_message_t *frame = last_frame(1, LINK_FRAME_NOTIFY);
    CHECK((frame->data[6] | (frame->data[7] << 8)) == 0);
    CHECK((frame->data[2] | (frame->data[3] << 8)) == 1); /* Second NOTIFY: sequence 1 */
}

static void a_seq_gap_makes_everyone_resync(void)
{
    start();
    open_session(0);
    attach_device();
    notify(10, 1);
    notify(11, 1);
    CHECK(last_frame(0, LINK_FRAME_RESYNC) == NULL);
    notify(14, 1); /* 12 and 13 were lost */
    const fake_message_t *resync = last_frame(0, LINK_FRAME_RESYNC);
    CHECK(resync != NULL && resync->data[5] == LINK_RESYNC_DROPPED);
}

static void a_full_queue_owes_a_resync(void)
{
    start();
    open_session(0);
    attach_device();
    io_record.queue_full[0] = true;
    notify(1, 1);
    CHECK(hub.device.notifications_dropped == 1);

    io_record.queue_full[0] = false;
    hub_tick(&hub);
    CHECK(last_frame(0, LINK_FRAME_RESYNC) != NULL);
}

/* ---------------------------------------------------------------------------
 * Polls (Link spec 7.6)
 * ------------------------------------------------------------------------- */

static const char *METER_POLL = "{\"t\":\"poll.subscribe\",\"id\":8,\"handle\":0,\"polls\":"
                                "[{\"slot\":%d,\"req\":80,\"val\":9,\"idx\":2,\"len\":41,"
                                "\"hz\":10}]}";

static void identical_polls_run_once_and_fan_out(void)
{
    start();
    open_session(0);
    open_session(1);
    attach_device();
    send_json(0, METER_POLL, 0);
    send_json(1, METER_POLL, 5);
    cJSON *ok = last_json(1, "ok");
    const cJSON *grant = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(ok, "granted"), 0);
    CHECK(json_number(grant, "slot") == 5 && json_number(grant, "hz") == 10);
    cJSON_Delete(ok);

    hub_tick(&hub);
    CHECK(io_record.submission_count == 1);
    CHECK(io_record.submissions[0].request.request == 80);
    hub_tick(&hub); /* One poll at a time */
    CHECK(io_record.submission_count == 1);

    static const uint8_t meters[41] = {7};
    answer_last(0, meters, sizeof(meters));
    const fake_message_t *poll0 = last_frame(0, LINK_FRAME_POLL);
    const fake_message_t *poll1 = last_frame(1, LINK_FRAME_POLL);
    CHECK(poll0 != NULL && poll0->data[5] == 0 && poll0->data[8] == 7);
    CHECK(poll1 != NULL && poll1->data[5] == 5);
}

static void a_slower_subscriber_gets_only_its_own_rate(void)
{
    start();
    open_session(0);
    open_session(1);
    attach_device();
    send_json(0, "{\"t\":\"poll.subscribe\",\"id\":8,\"handle\":0,\"polls\":"
                 "[{\"slot\":0,\"req\":80,\"val\":9,\"idx\":2,\"len\":41,\"hz\":20}]}");
    send_json(1, METER_POLL, 0); /* The same poll at 10 Hz */

    static const uint8_t meters[41] = {7};
    unsigned fast = 0;
    unsigned slow = 0;
    for (int tick = 0; tick < 20; tick++) { /* One second at 20 Hz */
        forget_messages();
        forget_submissions();
        hub_tick(&hub);
        if (io_record.submission_count == 1) {
            answer_last(0, meters, sizeof(meters));
        }
        fast += last_frame(0, LINK_FRAME_POLL) != NULL ? 1 : 0;
        slow += last_frame(1, LINK_FRAME_POLL) != NULL ? 1 : 0;
        io_record.now_ms += 50;
    }
    CHECK(fast == 20);
    CHECK(slow == 10);
}

static void commands_go_before_polls(void)
{
    start();
    open_session(0);
    attach_device();
    send_json(0, METER_POLL, 0);
    send_cmd(0, 1, true, 0x7F, 7, NULL);
    hub_tick(&hub);
    CHECK(io_record.submission_count == 1); /* Only the command */
}

static void polling_stops_when_nobody_subscribes(void)
{
    start();
    open_session(0);
    attach_device();
    send_json(0, METER_POLL, 0);
    send_json(0, "{\"t\":\"poll.unsubscribe\",\"id\":9,\"handle\":0,\"slots\":[0]}");
    io_record.now_ms += 1000;
    hub_tick(&hub);
    CHECK(io_record.submission_count == 0);
}

static void only_reads_may_be_polled(void)
{
    start();
    open_session(0);
    attach_device();
    send_json(0, "{\"t\":\"poll.subscribe\",\"id\":8,\"handle\":0,\"polls\":"
                 "[{\"slot\":0,\"req\":14,\"val\":0,\"idx\":2,\"len\":1,\"hz\":10}]}");
    cJSON *err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "code"), "denied") == 0);
    cJSON_Delete(err);
}

/* ---------------------------------------------------------------------------
 * Snapshot and locks
 * ------------------------------------------------------------------------- */

static void snapshots_come_from_the_cache_until_something_changes(void)
{
    start();
    open_session(0);
    attach_device();

    send_json(0, "{\"t\":\"device.snapshot\",\"id\":6,\"handle\":0}");
    CHECK(io_record.submission_count == 0); /* Served from the cache */
    cJSON *ok = last_json(0, "ok");
    CHECK(strlen(json_string(ok, "bulk_b64")) == 8184); /* 6136 bytes as base64 */
    CHECK(strlen(json_string(ok, "status_b64")) == 56);
    CHECK(json_number(ok, "wire_version") == 32);
    cJSON_Delete(ok);

    notify(1, 1);
    forget_messages();
    send_json(0, "{\"t\":\"device.snapshot\",\"id\":7,\"handle\":0}");
    CHECK(io_record.submission_count == 1); /* Stale: read again */
    CHECK(last_json(0, "ok") == NULL);
}

static void a_lock_keeps_other_sessions_out(void)
{
    start();
    const int holder = open_session(0);
    open_session(1);
    attach_device();
    send_json(0, "{\"t\":\"lock.acquire\",\"id\":10,\"handle\":0,\"timeout_ms\":90000}");
    cJSON *ok = last_json(0, "ok");
    CHECK(json_number(ok, "timeout_ms") == 60000); /* Capped */
    cJSON_Delete(ok);

    send_cmd(1, 1, false, 0xD2, 4, (const uint8_t[]){0, 0, 0xA0, 0xC1});
    CHECK(last_status(1) == LINK_STATUS_LOCKED);
    send_cmd(0, 1, false, 0xD2, 4, (const uint8_t[]){0, 0, 0xA0, 0xC1});
    CHECK(io_record.submission_count == 1); /* The holder may write */
    CHECK(hub.device.locked_by == holder);

    hub_session_closed(&hub, 0);
    CHECK(hub.device.locked_by == 0);
}

static void the_hub_can_be_renamed(void)
{
    start();
    open_session(0);
    send_json(0, "{\"t\":\"hub.rename\",\"id\":14,\"name\":\"\"}");
    cJSON *err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "code"), "bad_request") == 0);
    cJSON_Delete(err);

    send_json(0, "{\"t\":\"hub.rename\",\"id\":15,\"name\":\"Studio\"}");
    CHECK(strcmp(io_record.saved_hub_name, "Studio") == 0);
    CHECK(io_record.advertise_count >= 1);
}

void test_hub(void)
{
    RUN(hello_describes_the_bridge);
    RUN(the_first_message_must_be_hello);
    RUN(pairing_without_auth_reports_the_session);
    RUN(unknown_requests_get_unknown_type);
    RUN(a_full_bridge_has_no_room);
    RUN(keepalive_pings_then_closes_a_silent_session);
    RUN(a_session_that_never_says_hello_is_closed);
    RUN(a_found_device_is_announced_and_listed);
    RUN(a_lost_device_goes_offline_then_away);
    RUN(a_returning_device_triggers_resync);
    RUN(devices_can_be_renamed);
    RUN(a_get_goes_to_the_dspi_and_back);
    RUN(write_as_read_commands_are_never_retried);
    RUN(commands_without_a_device_are_refused);
    RUN(too_many_commands_are_rate_limited);
    RUN(long_sets_are_oversize_but_bulk_sets_go_through);
    RUN(failures_map_to_link_statuses);
    RUN(a_reply_for_a_closed_session_goes_nowhere);
    RUN(notifications_carry_the_writers_session);
    RUN(a_seq_gap_makes_everyone_resync);
    RUN(a_full_queue_owes_a_resync);
    RUN(identical_polls_run_once_and_fan_out);
    RUN(a_slower_subscriber_gets_only_its_own_rate);
    RUN(commands_go_before_polls);
    RUN(polling_stops_when_nobody_subscribes);
    RUN(only_reads_may_be_polled);
    RUN(snapshots_come_from_the_cache_until_something_changes);
    RUN(a_lock_keeps_other_sessions_out);
    RUN(the_hub_can_be_renamed);
}
