/*
 * test_hub.c - the hub state machine (Link spec 5 to 8, bridge spec 8),
 * driven through scripted sessions, a recording I/O and a fake clock.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "check.h"
#include "fake_crypto.h"
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

static uint32_t unix_now;

static uint32_t fake_unix_time(void *context)
{
    (void)context;
    return unix_now;
}

static uint32_t fake_auth_now(void *context)
{
    (void)context;
    return io_record.now_ms;
}

static void fake_auth_save(void *context, const hub_auth_store_t *store)
{
    (void)context;
    (void)store;
}

#define CLIENT_ADDRESS 0x0A01A8C0u /* 192.168.1.10 */

/* Starts the hub as hub_task does; `auth_unreadable` as when the saved PIN
 * could not be read. */
static void start_with(bool auth_unreadable)
{
    unix_now = 1800000000; /* 2027-01-15 */
    const hub_io_t io = fake_hub_io_init(&io_record);
    fake_crypto_reset();
    static const uint8_t MAC[6] = {0x80, 0x45, 0x6B, 0x35, 0x1B, 0x34};
    static const uint8_t RANDOM[16] = {1, 2, 3};
    hub_config_t config = {
        .board = &BOARD_TABLE_ESP32S3,
        .version = "0.1.0",
        .max_sessions = 4,
        .auth_unreadable = auth_unreadable,
    };
    settings_model_defaults(&config.settings, &BOARD_TABLE_ESP32S3, MAC, RANDOM);
    const hub_auth_io_t auth_io = {
        .random = fake_random,
        .sha256 = fake_sha256,
        .pbkdf2 = fake_pbkdf2,
        .now_ms = fake_auth_now,
        .unix_time = fake_unix_time,
        .save = fake_auth_save,
    };
    hub_init(&hub, &io, &config, &auth_io, NULL);
}

static void start(void)
{
    start_with(false);
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
    hub_session_opened(&hub, slot, CLIENT_ADDRESS);
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
    hub_session_opened(&hub, 0, CLIENT_ADDRESS);
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
    hub_session_opened(&hub, 0, CLIENT_ADDRESS);
    send_json(0, "{\"t\":\"device.list\",\"id\":1}");
    CHECK(io_record.close_code[0] == 4000);
}

/* JSON is parsed by recursion on a small stack before anyone has
 * authenticated, so deep nesting is refused rather than parsed. */
static void deeply_nested_json_is_refused(void)
{
    start();
    hub_session_opened(&hub, 0, CLIENT_ADDRESS);
    char deep[2 * 999 + 1];
    memset(deep, '[', 999);
    memset(deep + 999, ']', 999);
    deep[2 * 999] = '\0';
    hub_session_text(&hub, 0, deep, strlen(deep));
    CHECK(io_record.close_code[0] == 4000);

    /* The deepest message Link uses still parses. */
    start();
    open_session(0);
    send_json(0, "{\"t\":\"hub.config.set\",\"id\":2,\"config\":{\"uart\":{\"baud\":921600}}}");
    CHECK(io_record.apply_count == 1);
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
        hub_session_opened(&hub, slot, CLIENT_ADDRESS);
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
    hub_session_opened(&hub, 0, CLIENT_ADDRESS);
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
    CHECK(last_status(0) == LINK_STATUS_TIMEOUT); /* It may have run: never "retry" */

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

static void answer_tag(uint32_t tag, uint8_t status, const uint8_t *payload, uint16_t length);

/* The request of the last submission, or -1 if there is none. */
static int last_request(void)
{
    return io_record.submission_count > 0
               ? io_record.submissions[io_record.submission_count - 1].request.request
               : -1;
}

static void snapshots_read_the_status_fresh_and_the_blob_when_it_changed(void)
{
    start();
    open_session(0);
    attach_device();

    /* The blob is current, so only the status frame is read (bridge spec
     * 8.6), and the answer waits for it. */
    forget_messages();
    send_json(0, "{\"t\":\"device.snapshot\",\"id\":6,\"handle\":0}");
    CHECK(io_record.submission_count == 1 && last_request() == 0x50);
    CHECK(last_json(0, "ok") == NULL);
    static const uint8_t status[41] = {7, 7, 7};
    answer_last(0, status, sizeof(status));
    cJSON *ok = last_json(0, "ok");
    CHECK(strlen(json_string(ok, "bulk_b64")) == 8184);            /* 6136 bytes as base64 */
    CHECK(strncmp(json_string(ok, "status_b64"), "BwcH", 4) == 0); /* The new frame */
    CHECK(json_number(ok, "wire_version") == 32);
    cJSON_Delete(ok);

    notify(1, 1);
    forget_messages();
    forget_submissions();
    send_json(0, "{\"t\":\"device.snapshot\",\"id\":7,\"handle\":0}");
    CHECK(io_record.submission_count == 1 && last_request() == 0xA0); /* Stale: blob too */
    CHECK(last_json(0, "ok") == NULL);
}

static void a_snapshot_asked_during_a_status_read_waits_for_the_next(void)
{
    start();
    open_session(0);
    open_session(1);
    attach_device();
    forget_messages();
    send_json(0, "{\"t\":\"device.snapshot\",\"id\":6,\"handle\":0}");
    send_json(1, "{\"t\":\"device.snapshot\",\"id\":7,\"handle\":0}");
    CHECK(io_record.submission_count == 1); /* One read at a time */
    static const uint8_t status[41] = {1};
    answer_last(0, status, sizeof(status));
    cJSON *ok = last_json(0, "ok");
    CHECK(ok != NULL);
    cJSON_Delete(ok);
    CHECK(last_json(1, "ok") == NULL); /* Its read had started before it asked */
    CHECK(io_record.submission_count == 2 && last_request() == 0x50);
    answer_last(0, status, sizeof(status));
    ok = last_json(1, "ok");
    CHECK(ok != NULL && json_number(ok, "id") == 7);
    cJSON_Delete(ok);
}

static void without_notifications_every_snapshot_reads_the_blob(void)
{
    start();
    open_session(0);
    dspi_identity_t quiet = IDENTITY;
    quiet.notify_enabled = false;
    hub_device_found(&hub, &quiet, 1000000);
    static uint8_t bulk[6136] = {32, 0, 17};
    answer_last(0, bulk, sizeof(bulk));
    static const uint8_t status[41] = {1};
    answer_last(0, status, sizeof(status));
    forget_submissions();

    /* Nothing would tell the cache the DSPi changed, so it is never trusted. */
    send_json(0, "{\"t\":\"device.snapshot\",\"id\":6,\"handle\":0}");
    CHECK(io_record.submission_count == 1 && last_request() == 0xA0);
}

static void write_as_read_commands_mark_the_cache_stale(void)
{
    start();
    open_session(0);
    attach_device();
    send_cmd(0, 1, true, 0x91 /* PRESET_LOAD, a GET that writes */, 1, NULL);
    answer_last(0, (const uint8_t[]){0}, 1);
    forget_submissions();
    send_json(0, "{\"t\":\"device.snapshot\",\"id\":6,\"handle\":0}");
    CHECK(io_record.submission_count == 1 && last_request() == 0xA0);
}

static void an_answer_for_an_abandoned_snapshot_read_is_ignored(void)
{
    start();
    open_session(0);
    attach_device();
    notify(1, 1);
    send_json(0, "{\"t\":\"device.snapshot\",\"id\":6,\"handle\":0}");
    const uint32_t old_read = io_record.submissions[io_record.submission_count - 1].tag;
    hub_device_lost(&hub);
    hub_device_found(&hub, &IDENTITY, 1000000); /* Reads the blob again */
    const uint32_t new_read = io_record.submissions[io_record.submission_count - 1].tag;
    send_json(0, "{\"t\":\"device.snapshot\",\"id\":7,\"handle\":0}");
    forget_messages();

    /* The old read's late failure neither fails the new waiter nor ends the
     * new read. */
    answer_tag(old_read, 2, NULL, 0);
    CHECK(last_json(0, "err") == NULL);
    static uint8_t bulk[6136] = {32, 0, 17};
    answer_tag(new_read, 0, bulk, sizeof(bulk));
    static const uint8_t status[41] = {1};
    answer_last(0, status, sizeof(status));
    cJSON *ok = last_json(0, "ok");
    CHECK(ok != NULL && json_number(ok, "id") == 7);
    cJSON_Delete(ok);
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
    CHECK(strcmp(io_record.applied.name, "Studio") == 0);
    CHECK(io_record.advertise_count >= 1);
}

/* ---------------------------------------------------------------------------
 * The PIN (Link spec 6.2, bridge spec 8.9)
 * ------------------------------------------------------------------------- */

/* Says hello on `slot` with the PIN on; returns nothing, leaves the session
 * waiting to authenticate. */
static void hello_only(int slot)
{
    hub_session_opened(&hub, slot, CLIENT_ADDRESS);
    send_json(slot, "{\"t\":\"hello\",\"proto\":{\"major\":1,\"minor\":2}}");
}

static void with_the_pin_on_hello_asks_for_it(void)
{
    start();
    hub_set_pin(&hub, "482913");
    hello_only(0);
    cJSON *hello = last_json(0, "hello");
    CHECK(strcmp(json_string(hello, "auth"), "pin") == 0);
    CHECK(strcmp(json_string(hello, "pin_source"), "fixed") == 0);
    cJSON_Delete(hello);

    send_json(0, "{\"t\":\"device.list\",\"id\":2}");
    cJSON *err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "code"), "unauthenticated") == 0);
    cJSON_Delete(err);
}

static void pairing_with_the_pin_issues_a_token(void)
{
    start();
    hub_set_pin(&hub, "482913");
    hello_only(0);
    send_json(0, "{\"t\":\"auth.pair\",\"id\":1,\"pin\":\"111111\",\"name\":\"Phone\"}");
    cJSON *err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "code"), "unauthenticated") == 0);
    cJSON_Delete(err);

    send_json(0, "{\"t\":\"auth.pair\",\"id\":2,\"pin\":\"482913\",\"name\":\"Phone\"}");
    cJSON *ok = last_json(0, "ok");
    char token[64];
    snprintf(token, sizeof(token), "%s", json_string(ok, "token"));
    CHECK(strlen(token) == HUB_AUTH_TOKEN_CHARS);
    CHECK(strcmp(json_string(ok, "role"), "admin") == 0);
    cJSON_Delete(ok);

    /* A later session presents the token instead. */
    hello_only(1);
    send_json(1, "{\"t\":\"auth.token\",\"id\":1,\"token\":\"%s\"}", token);
    ok = last_json(1, "ok");
    CHECK(ok != NULL && json_number(ok, "session") > 0);
    cJSON_Delete(ok);
    send_json(1, "{\"t\":\"device.list\",\"id\":2}");
    ok = last_json(1, "ok");
    CHECK(cJSON_GetObjectItemCaseSensitive(ok, "devices") != NULL);
    cJSON_Delete(ok);
}

static void setting_the_pin_keeps_the_setter_and_signs_out_the_rest(void)
{
    start();
    open_session(0);
    open_session(1);
    send_json(0, "{\"t\":\"hub.auth.set\",\"id\":5,\"mode\":\"pin\",\"pin\":\"482913\"}");
    cJSON *ok = last_json(0, "ok");
    CHECK(strlen(json_string(ok, "token")) == HUB_AUTH_TOKEN_CHARS);
    cJSON_Delete(ok);
    CHECK(io_record.close_code[0] == 0);
    CHECK(io_record.close_code[1] == 4002);
    CHECK(hub_pin_enabled(&hub));

    send_json(0, "{\"t\":\"hub.auth.set\",\"id\":6,\"mode\":\"pin\",\"pin\":\"12ab56\"}");
    cJSON *err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "code"), "bad_request") == 0);
    cJSON_Delete(err);

    send_json(0, "{\"t\":\"hub.auth.set\",\"id\":7,\"mode\":\"none\"}");
    CHECK(!hub_pin_enabled(&hub));
}

static void a_revoked_client_is_disconnected(void)
{
    start();
    hub_set_pin(&hub, "482913");
    hello_only(0);
    send_json(0, "{\"t\":\"auth.pair\",\"id\":1,\"pin\":\"482913\",\"name\":\"Admin\"}");
    hello_only(1);
    send_json(1, "{\"t\":\"auth.pair\",\"id\":1,\"pin\":\"482913\",\"name\":\"Tablet\"}");

    send_json(0, "{\"t\":\"auth.list\",\"id\":2}");
    cJSON *ok = last_json(0, "ok");
    const cJSON *clients = cJSON_GetObjectItemCaseSensitive(ok, "clients");
    CHECK(cJSON_GetArraySize(clients) == 2);
    int tablet = -1;
    const cJSON *client = NULL;
    cJSON_ArrayForEach(client, clients)
    {
        if (strcmp(json_string(client, "name"), "Tablet") == 0) {
            tablet = json_number(client, "cid");
            CHECK(strcmp(json_string(client, "created"), "2027-01-15T08:00:00Z") == 0);
            CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(client, "online")));
        }
    }
    cJSON_Delete(ok);

    send_json(0, "{\"t\":\"auth.revoke\",\"id\":3,\"cid\":%d}", tablet);
    CHECK(io_record.close_code[1] == 4002);
    CHECK(io_record.close_code[0] == 0);
}

/* ---------------------------------------------------------------------------
 * Administration (Link spec 7.10 to 7.13)
 * ------------------------------------------------------------------------- */

static void config_get_shows_settings_and_choices(void)
{
    start();
    open_session(0);
    send_json(0, "{\"t\":\"hub.config.get\",\"id\":20}");
    cJSON *ok = last_json(0, "ok");
    const cJSON *config = cJSON_GetObjectItemCaseSensitive(ok, "config");
    const cJSON *uart = cJSON_GetObjectItemCaseSensitive(config, "uart");
    CHECK(json_number(uart, "tx_pin") == 17);
    CHECK(strcmp(json_string(config, "hostname"), "dspi-bridge-1b34") == 0);
    const cJSON *options = cJSON_GetObjectItemCaseSensitive(ok, "options");
    const cJSON *tx_choices = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(options, "uart"), "tx_pin");
    CHECK(cJSON_GetArraySize(tx_choices) == (int)BOARD_TABLE_ESP32S3.uart_tx_pins.count);
    cJSON_Delete(ok);
}

static void config_set_applies_whole_changes_only(void)
{
    start();
    open_session(0);
    open_session(1);
    send_json(0, "{\"t\":\"hub.config.set\",\"id\":21,\"config\":{\"uart\":{\"baud\":921600}}}");
    CHECK(io_record.apply_count == 1);
    CHECK(io_record.applied.uart_baud == 921600);
    CHECK(io_record.applied.uart_tx_pin == 17); /* Untouched keys keep their values */
    cJSON *changed = last_json(1, "hub.config.changed");
    CHECK(changed != NULL);
    cJSON_Delete(changed);

    send_json(0, "{\"t\":\"hub.config.set\",\"id\":22,\"config\":{\"uart\":{\"tx_pin\":19}}}");
    cJSON *err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "field"), "uart.tx_pin") == 0); /* USB on the S3 */
    cJSON_Delete(err);

    send_json(0, "{\"t\":\"hub.config.set\",\"id\":23,\"config\":{\"colour\":\"red\"}}");
    err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "field"), "colour") == 0); /* Unknown keys are errors */
    cJSON_Delete(err);
    CHECK(io_record.apply_count == 1);
}

static void a_detected_rate_is_kept_and_shown(void)
{
    start();
    open_session(0);
    hub_device_found(&hub, &IDENTITY, 921600); /* The settings say 1,000,000 */
    CHECK(io_record.apply_count == 1);
    CHECK(io_record.applied.uart_baud == 921600);
    cJSON *changed = last_json(0, "hub.config.changed");
    CHECK(json_number(cJSON_GetObjectItemCaseSensitive(
                          cJSON_GetObjectItemCaseSensitive(changed, "config"), "uart"),
                      "baud") == 921600);
    cJSON_Delete(changed);

    hub_device_lost(&hub);
    hub_device_found(&hub, &IDENTITY, 921600); /* Same rate again: nothing to save */
    CHECK(io_record.apply_count == 1);
}

static void wifi_scans_answer_later(void)
{
    start();
    open_session(0);
    forget_messages(); /* Drop the auth reply, so "no ok yet" means something */
    send_json(0, "{\"t\":\"hub.wifi.scan\",\"id\":30}");
    CHECK(io_record.scan_slot == 0 && io_record.scan_request == 30);
    CHECK(last_json(0, "ok") == NULL);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddArrayToObject(body, "networks");
    hub_reply_later(&hub, io_record.scan_slot, io_record.scan_session, io_record.scan_request,
                    body);
    cJSON *ok = last_json(0, "ok");
    CHECK(json_number(ok, "id") == 30);
    cJSON_Delete(ok);
}

static void joining_another_network_closes_every_session(void)
{
    start();
    open_session(0);
    open_session(1);
    io_record.refuse_join = true;
    send_json(0, "{\"t\":\"hub.wifi.join\",\"id\":31,\"ssid\":\"Studio\",\"password\":\"x\"}");
    cJSON *err = last_json(0, "err");
    CHECK(err != NULL);
    cJSON_Delete(err);

    io_record.refuse_join = false;
    send_json(0, "{\"t\":\"hub.wifi.join\",\"id\":32,\"ssid\":\"Studio\","
                 "\"password\":\"studio-pass\"}");
    CHECK(strcmp(io_record.joined_ssid, "Studio") == 0);
    CHECK(io_record.close_code[0] == 4003 && io_record.close_code[1] == 4003);
}

static void a_factory_reset_tells_everyone_then_restarts(void)
{
    start();
    open_session(0);
    send_json(0, "{\"t\":\"hub.reset\",\"id\":33,\"level\":\"sideways\"}");
    CHECK(io_record.restart_level == -1);
    send_json(0, "{\"t\":\"hub.reset\",\"id\":34,\"level\":\"factory\"}");
    CHECK(last_json(0, "ok") != NULL);
    CHECK(io_record.close_code[0] == 4003);
    CHECK(io_record.restart_level == HUB_RESTART_FACTORY);
}

/* ---------------------------------------------------------------------------
 * Firmware update (Link spec 7.14, bridge spec 14.2)
 * ------------------------------------------------------------------------- */

#define TEST_SHA256 "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"

/* Sends one FWDATA chunk of `length` bytes at `offset` (Link spec 8.5). */
static void send_chunk(int slot, uint16_t transfer, uint32_t offset, size_t length)
{
    uint8_t frame[8 + 64] = {LINK_FRAME_FWDATA,       0,
                             (uint8_t)transfer,       (uint8_t)(transfer >> 8),
                             (uint8_t)offset,         (uint8_t)(offset >> 8),
                             (uint8_t)(offset >> 16), (uint8_t)(offset >> 24)};
    hub_session_binary(&hub, slot, frame, 8 + length);
}

/* The status of the last FWDATA ack sent to `slot` for `offset`, or -1. */
static int last_ack_status(int slot, uint32_t offset)
{
    const fake_message_t *ack = last_frame(slot, LINK_FRAME_FWDATA_ACK);
    if (ack == NULL || ack->length < 9) {
        return -1;
    }
    const uint32_t acked = (uint32_t)ack->data[4] | (uint32_t)ack->data[5] << 8 |
                           (uint32_t)ack->data[6] << 16 | (uint32_t)ack->data[7] << 24;
    return acked == offset ? ack->data[8] : -1;
}

/* True when the hello sent to `slot` offers the capability `name`. */
static bool hello_offers(int slot, const char *name)
{
    cJSON *hello = last_json(slot, "hello");
    bool found = false;
    const cJSON *cap;
    cJSON_ArrayForEach(cap, cJSON_GetObjectItemCaseSensitive(hello, "caps"))
    {
        found = found || (cJSON_IsString(cap) && strcmp(cap->valuestring, name) == 0);
    }
    cJSON_Delete(hello);
    return found;
}

/* Starts a 10-byte update from `slot` and returns its transfer id. */
static uint16_t begin_update(int slot)
{
    send_json(slot, "{\"t\":\"hub.update\",\"id\":40,\"size\":10,\"sha256\":\"" TEST_SHA256 "\"}");
    cJSON *ok = last_json(slot, "ok");
    const int transfer = json_number(ok, "xfer");
    cJSON_Delete(ok);
    return (uint16_t)transfer;
}

static void an_update_is_written_chunk_by_chunk_then_restarts(void)
{
    start();
    open_session(0);
    open_session(1);
    CHECK(hello_offers(0, "hub_update"));

    const uint16_t transfer = begin_update(0);
    CHECK(transfer != 0);
    CHECK(io_record.update_size == 10);
    CHECK(io_record.update_sha256[1] == 0x11 && io_record.update_sha256[31] == 0xff);

    forget_messages();
    send_chunk(0, transfer, 0, 6);
    CHECK(io_record.update_writes == 1);
    CHECK(last_ack_status(0, 0) == -1); /* Acknowledged only once written */
    hub_update_written(&hub, io_record.update_generation, 0);
    CHECK(last_ack_status(0, 0) == 0);

    send_chunk(0, transfer, 6, 4);
    hub_update_written(&hub, io_record.update_generation, 6);
    CHECK(last_ack_status(0, 6) == 0);
    CHECK(io_record.update_bytes_written == 10);

    hub_update_finished(&hub, io_record.update_generation, true, "0.2.0");
    cJSON *done = last_json(0, "hub.update.done");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(done, "ok")));
    CHECK(strcmp(json_string(done, "version"), "0.2.0") == 0);
    cJSON_Delete(done);
    CHECK(io_record.close_code[0] == 4003 && io_record.close_code[1] == 4003);
    CHECK(io_record.restart_level == HUB_RESTART_ONLY);
}

static void bad_updates_are_refused_and_keep_the_firmware(void)
{
    start();
    open_session(0);
    open_session(1);

    send_json(0,
              "{\"t\":\"hub.update\",\"id\":41,\"size\":99999999,\"sha256\":\"" TEST_SHA256 "\"}");
    cJSON *err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "code"), "bad_request") == 0); /* Too large */
    cJSON_Delete(err);
    forget_messages();
    send_json(0, "{\"t\":\"hub.update\",\"id\":42,\"size\":10,\"sha256\":\"+0112233\"}");
    err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "code"), "bad_request") == 0); /* Not a SHA-256 */
    cJSON_Delete(err);

    const uint16_t transfer = begin_update(0);
    forget_messages();
    send_json(1, "{\"t\":\"hub.update\",\"id\":43,\"size\":10,\"sha256\":\"" TEST_SHA256 "\"}");
    err = last_json(1, "err");
    CHECK(strcmp(json_string(err, "code"), "busy") == 0); /* One at a time */
    cJSON_Delete(err);

    send_chunk(1, transfer, 0, 4); /* Someone else's chunk is refused... */
    CHECK(last_ack_status(1, 0) == 1);
    CHECK(io_record.update_writes == 0);

    send_chunk(0, transfer, 4, 4); /* ...and a gap ends the update */
    CHECK(last_ack_status(0, 4) == 1);
    cJSON *done = last_json(0, "hub.update.done");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(done, "ok")));
    CHECK(strlen(json_string(done, "msg")) > 0);
    cJSON_Delete(done);
    CHECK(io_record.update_aborts == 1);
    CHECK(io_record.restart_level == -1);

    CHECK(begin_update(0) != transfer); /* A new update may start */
    forget_messages();
    hub_update_finished(&hub, io_record.update_generation, false,
                        "this file is not firmware for this chip");
    done = last_json(0, "hub.update.done"); /* The writer's reason reaches the client */
    CHECK(strcmp(json_string(done, "msg"), "this file is not firmware for this chip") == 0);
    cJSON_Delete(done);
    CHECK(io_record.restart_level == -1);
}

static void a_complete_update_survives_its_sender_leaving(void)
{
    start();
    open_session(0);
    const uint16_t transfer = begin_update(0);
    send_chunk(0, transfer, 0, 6);
    send_chunk(0, transfer, 6, 4); /* Every byte is in: only the check is left */
    hub_session_closed(&hub, 0);
    CHECK(io_record.update_aborts == 0);
    hub_update_finished(&hub, io_record.update_generation, true, "0.2.0");
    CHECK(io_record.restart_level == HUB_RESTART_ONLY);
}

static void reports_from_an_abandoned_update_are_ignored(void)
{
    start();
    open_session(0);
    const uint16_t first = begin_update(0);
    const uint32_t first_generation = io_record.update_generation;
    send_chunk(0, first, 0, 4);
    send_chunk(0, first, 8, 2); /* A gap ends the first update */
    CHECK(io_record.update_aborts == 1);

    const uint16_t second = begin_update(0);
    forget_messages();
    hub_update_written(&hub, first_generation, 0); /* The first's late report */
    CHECK(last_ack_status(0, 0) == -1);
    hub_update_finished(&hub, first_generation, false, "late failure");
    CHECK(last_json(0, "hub.update.done") == NULL);

    send_chunk(0, second, 0, 6); /* The second carries on */
    hub_update_written(&hub, io_record.update_generation, 0);
    CHECK(last_ack_status(0, 0) == 0);
}

static void a_stalled_or_orphaned_update_is_abandoned(void)
{
    start();
    open_session(0);
    begin_update(0);
    for (int i = 0; i < 7; i++) {
        io_record.now_ms += 5000;
        hub_session_activity(&hub, 0); /* The session stays alive; the update does not */
        hub_tick(&hub);
    }
    CHECK(io_record.update_aborts == 1);
    cJSON *done = last_json(0, "hub.update.done");
    CHECK(done != NULL);
    cJSON_Delete(done);

    begin_update(0);
    hub_session_closed(&hub, 0);
    CHECK(io_record.update_aborts == 2);

    start();
    io_record.update_capacity = 0; /* A build that cannot update */
    open_session(0);
    CHECK(!hello_offers(0, "hub_update"));
}

/* ---------------------------------------------------------------------------
 * Fixes from the code review: lost answers, poll limits, snapshot waiters
 * and failures the I/O reports
 * ------------------------------------------------------------------------- */

/* Answers the request submitted with `tag`. */
static void answer_tag(uint32_t tag, uint8_t status, const uint8_t *payload, uint16_t length)
{
    const dspi_reply_t reply = {
        .kind = DSPI_REPLY_ANSWERED, .status = status, .payload = payload, .length = length};
    hub_reply(&hub, tag, &reply);
}

static void a_lost_answer_times_out_and_a_late_one_is_ignored(void)
{
    start();
    open_session(0);
    attach_device();
    send_cmd(0, 1, true, 0x7F, 7, NULL);
    send_cmd(0, 2, true, 0x7F, 7, NULL);
    const uint32_t first = io_record.submissions[0].tag;
    const uint32_t second = io_record.submissions[1].tag;

    io_record.now_ms += HUB_REPLY_LOST_AFTER_MS;
    hub_session_activity(&hub, 0);
    hub_tick(&hub);
    const fake_message_t *response = last_frame(0, LINK_FRAME_CMD_RESPONSE);
    CHECK(response != NULL && response->data[2] == 1 && response->data[4] == LINK_STATUS_TIMEOUT);
    CHECK(hub.sessions[0].commands_in_flight == 1);

    /* The first answer turns up after all: it must not finish the second. */
    forget_messages();
    answer_tag(first, 0, (const uint8_t[]){1, 2, 3}, 3);
    CHECK(last_frame(0, LINK_FRAME_CMD_RESPONSE) == NULL);
    CHECK(hub.pending_count == 1);

    answer_tag(second, 0, (const uint8_t[]){4, 5, 6}, 3);
    response = last_frame(0, LINK_FRAME_CMD_RESPONSE);
    CHECK(response != NULL && response->data[2] == 2 && response->data[4] == 0);
    CHECK(hub.pending_count == 0 && hub.client_commands_pending == 0);
}

static void an_unknown_answer_finishes_only_older_requests(void)
{
    start();
    open_session(0);
    attach_device();
    send_cmd(0, 1, true, 0x7F, 7, NULL);
    send_cmd(0, 2, true, 0x7F, 7, NULL);
    const uint32_t second = io_record.submissions[1].tag;

    /* An answer for a tag before every pending one changes nothing. */
    answer_tag(io_record.submissions[0].tag - 1, 0, NULL, 0);
    CHECK(hub.pending_count == 2);

    /* The second's answer means the first's was lost on the way. */
    answer_tag(second, 0, (const uint8_t[]){9}, 1);
    CHECK(hub.pending_count == 0);
    CHECK(hub.sessions[0].commands_in_flight == 0);
}

/* Subscribes `count` polls on `slot`, slots `first_slot` up, to distinct
 * keys starting at index `first_index` (or all to `first_index` when
 * `same` is set). */
static void subscribe_polls(int slot, int first_slot, int count, int first_index, bool same)
{
    char text[2048];
    size_t used = (size_t)snprintf(text, sizeof(text),
                                   "{\"t\":\"poll.subscribe\",\"id\":8,\"handle\":0,\"polls\":[");
    for (int i = 0; i < count; i++) {
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                 "%s{\"slot\":%d,\"req\":80,\"val\":9,\"idx\":%d,\"len\":41,"
                                 "\"hz\":1}",
                                 i > 0 ? "," : "", first_slot + i, first_index + (same ? 0 : i));
    }
    snprintf(text + used, sizeof(text) - used, "]}");
    hub_session_text(&hub, slot, text, strlen(text));
}

static void a_refused_subscription_changes_nothing(void)
{
    start();
    open_session(0);
    open_session(1);
    open_session(2);
    attach_device();
    subscribe_polls(1, 0, 1, 0, false);    /* Shares session 0's first poll below */
    subscribe_polls(0, 0, 16, 0, false);   /* 16 groups */
    subscribe_polls(2, 0, 16, 100, false); /* 32: the most there may be */
    CHECK(hub.poll_group_count == HUB_MAX_POLL_GROUPS);

    forget_messages();
    subscribe_polls(1, 0, 1, 200, false); /* Would need a 33rd group */
    cJSON *err = last_json(1, "err");
    CHECK(strcmp(json_string(err, "code"), "busy") == 0);
    cJSON_Delete(err);
    CHECK(hub.sessions[1].polls[0].active && hub.sessions[1].polls[0].key.index == 0);
    CHECK(hub.poll_group_count == HUB_MAX_POLL_GROUPS);
}

static void an_update_holds_back_large_answers_until_it_ends(void)
{
    start();
    open_session(0);
    open_session(1);
    attach_device();
    subscribe_polls(1, 0, 1, 0, false);
    hub_tick(&hub);
    CHECK(io_record.submission_count == 1); /* The poll runs... */
    static const uint8_t status[41] = {1};
    answer_last(0, status, sizeof(status));
    const uint16_t transfer = begin_update(0);
    forget_messages();
    forget_submissions();

    io_record.now_ms += 2000;
    hub_tick(&hub);
    CHECK(io_record.submission_count == 0); /* ...but waits during the update */
    send_json(1, "{\"t\":\"device.snapshot\",\"id\":6,\"handle\":0}");
    CHECK(io_record.submission_count == 0);                           /* So does the snapshot... */
    CHECK(last_json(1, "ok") == NULL && last_json(1, "err") == NULL); /* ...unanswered */
    send_cmd(1, 5, true, 0xA0, 6136, NULL);
    CHECK(last_status(1) == DSPI_STATUS_BUSY); /* A large GET is not run */
    CHECK(io_record.submission_count == 0);
    send_cmd(1, 6, true, 0x7F, 7, NULL);
    CHECK(io_record.submission_count == 1); /* A small one goes on */
    answer_last(0, (const uint8_t[]){1, 2, 3, 4, 5, 6, 7}, 7);

    forget_submissions();
    send_chunk(0, transfer, 4, 2); /* A gap ends the update */
    CHECK(io_record.update_aborts == 1);
    CHECK(io_record.submission_count == 1 && last_request() == 0x50); /* The snapshot's read */
    answer_last(0, status, sizeof(status));
    cJSON *ok = last_json(1, "ok");
    CHECK(ok != NULL);
    cJSON_Delete(ok);
    hub_tick(&hub);
    CHECK(io_record.submission_count == 2); /* Polls run again */
}

static void repeated_keys_need_one_group(void)
{
    start();
    open_session(0);
    open_session(1);
    open_session(2);
    attach_device();
    subscribe_polls(0, 0, 16, 0, false);
    subscribe_polls(2, 0, 15, 100, false); /* 31 groups */
    forget_messages();
    subscribe_polls(1, 0, 2, 200, true); /* Two slots, one new key: 32 */
    cJSON *ok = last_json(1, "ok");
    CHECK(ok != NULL);
    cJSON_Delete(ok);
    CHECK(hub.poll_group_count == HUB_MAX_POLL_GROUPS);
}

static void a_failing_poll_reports_once(void)
{
    start();
    open_session(0);
    attach_device();
    send_json(0, METER_POLL, 0);
    unsigned errors = 0;
    for (int tick = 0; tick < 300; tick++) { /* Past where a byte counter wraps */
        forget_messages();
        forget_submissions();
        hub_session_activity(&hub, 0);
        hub_tick(&hub);
        if (io_record.submission_count == 1) {
            const dspi_reply_t timeout = {.kind = DSPI_REPLY_TIMEOUT};
            hub_reply(&hub, io_record.submissions[0].tag, &timeout);
        }
        cJSON *event = last_json(0, "poll.error");
        errors += event != NULL ? 1 : 0;
        cJSON_Delete(event);
        io_record.now_ms += HUB_POLL_FAILED_PERIOD_MS;
    }
    CHECK(errors == 1);
}

/* How many `ok` replies with `id` were sent to `slot`. */
static unsigned count_ok(int slot, int id)
{
    unsigned count = 0;
    for (size_t i = 0; i < io_record.message_count; i++) {
        const fake_message_t *message = &io_record.messages[i];
        if (message->slot != slot || message->binary) {
            continue;
        }
        cJSON *json = cJSON_ParseWithLength((const char *)message->data, message->length);
        count += strcmp(json_string(json, "t"), "ok") == 0 && json_number(json, "id") == id;
        cJSON_Delete(json);
    }
    return count;
}

static void one_session_cannot_fill_the_snapshot_queue(void)
{
    start();
    open_session(0);
    open_session(1);
    attach_device();
    notify(1, 1); /* The cache is stale */
    forget_messages();
    for (int id = 1; id <= HUB_MAX_SESSIONS + 1; id++) {
        send_json(0, "{\"t\":\"device.snapshot\",\"id\":%d,\"handle\":0}", id);
    }
    cJSON *err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "code"), "busy") == 0); /* Only the first waits */
    cJSON_Delete(err);
    send_json(1, "{\"t\":\"device.snapshot\",\"id\":50,\"handle\":0}");
    CHECK(last_json(1, "err") == NULL);
    CHECK(hub.snapshot.waiter_count == 2);

    static uint8_t bulk[6136] = {32, 1, 17, 9, 8};
    answer_last(0, bulk, sizeof(bulk));
    static const uint8_t status[41] = {1};
    answer_last(0, status, sizeof(status));
    CHECK(count_ok(0, 1) == 1);
    CHECK(count_ok(0, 2) == 0);
    CHECK(count_ok(1, 50) == 1);
}

static void failures_the_io_reports_are_errors(void)
{
    start();
    open_session(0);
    open_session(1);
    attach_device();

    io_record.refuse_apply = true;
    forget_messages();
    send_json(0, "{\"t\":\"hub.config.set\",\"id\":21,\"config\":{\"uart\":{\"baud\":921600}}}");
    cJSON *err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "code"), "internal") == 0);
    cJSON_Delete(err);
    CHECK(last_json(1, "hub.config.changed") == NULL);
    CHECK(hub.config.settings.uart_baud == 1000000);

    send_json(0, "{\"t\":\"hub.rename\",\"id\":22,\"name\":\"Studio\"}");
    err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "code"), "internal") == 0);
    cJSON_Delete(err);
    CHECK(hub_rename(&hub, "Studio") == HUB_CHANGE_FAILED);

    io_record.refuse_save_name = true;
    send_json(0, "{\"t\":\"device.rename\",\"id\":23,\"handle\":0,\"name\":\"Kitchen\"}");
    err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "code"), "internal") == 0);
    cJSON_Delete(err);
    CHECK(strcmp(hub.device.name, "Kitchen") != 0);

    io_record.scan_result = HUB_IO_BUSY;
    send_json(0, "{\"t\":\"hub.wifi.scan\",\"id\":24}");
    err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "code"), "busy") == 0);
    cJSON_Delete(err);
    io_record.scan_result = HUB_IO_FAILED;
    send_json(0, "{\"t\":\"hub.wifi.scan\",\"id\":25}");
    err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "code"), "internal") == 0 && json_number(err, "id") == 25);
    cJSON_Delete(err);
}

static void a_failed_hash_keeps_the_pin_off(void)
{
    start();
    open_session(0);
    fake_pbkdf2_fails = true;
    send_json(0, "{\"t\":\"hub.auth.set\",\"id\":5,\"mode\":\"pin\",\"pin\":\"482913\"}");
    cJSON *err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "code"), "internal") == 0);
    cJSON_Delete(err);
    CHECK(!hub_pin_enabled(&hub));
    CHECK(hub_set_pin(&hub, "482913") == HUB_AUTH_UNAVAILABLE);
    CHECK(io_record.close_code[0] == 0);
}

/* ---------------------------------------------------------------------------
 * Origins, the poll budget, names, roles and other audit findings
 * ------------------------------------------------------------------------- */

static uint16_t last_notify_origin(int slot)
{
    const fake_message_t *frame = last_frame(slot, LINK_FRAME_NOTIFY);
    return frame != NULL ? (uint16_t)(frame->data[6] | (frame->data[7] << 8)) : 0xFFFF;
}

static void a_refused_write_claims_no_notification(void)
{
    start();
    open_session(0);
    const int second = open_session(1);
    attach_device();
    send_cmd(0, 1, false, 0xD2, 4, (const uint8_t[]){0, 0, 0xA0, 0xC1});
    answer_last(2 /* ERROR: nothing changed */, NULL, 0);
    send_cmd(1, 1, false, 0xD2, 4, (const uint8_t[]){0, 0, 0xA0, 0xC1});
    answer_last(0, NULL, 0);
    notify(1, 1 /* HOST_SET */);
    CHECK(last_notify_origin(0) == second);
}

static void write_as_read_commands_are_attributed(void)
{
    start();
    const int loader = open_session(0);
    attach_device();
    send_cmd(0, 1, true, 0x91 /* PRESET_LOAD */, 1, NULL);
    answer_last(0, (const uint8_t[]){0}, 1);
    notify(1, 1);
    CHECK(last_notify_origin(0) == loader);
}

static void a_write_queued_behind_a_slow_one_keeps_its_window(void)
{
    start();
    const int writer = open_session(0);
    attach_device();
    send_cmd(0, 1, true, 0x7F, 7, NULL); /* A read first */
    const uint32_t read = io_record.submissions[0].tag;
    send_cmd(0, 2, false, 0xD2, 4, (const uint8_t[]){0, 0, 0xA0, 0xC1});
    const uint32_t write = io_record.submissions[1].tag;
    io_record.now_ms += 2 * LINK_ORIGIN_WINDOW_MS; /* The read takes its time */
    answer_tag(read, 0, (const uint8_t[]){1}, 1);
    io_record.now_ms += 10;
    answer_tag(write, 0, NULL, 0);
    notify(1, 1);
    /* The window runs from when the DSPi was asked, not from when it was queued. */
    CHECK(last_notify_origin(0) == writer);
}

static void subscribe_one(int slot, int poll_slot, int request, int value, int length, int hz)
{
    send_json(slot,
              "{\"t\":\"poll.subscribe\",\"id\":30,\"handle\":0,\"polls\":[{\"slot\":%d,\"req\":%d,"
              "\"val\":%d,\"idx\":2,\"len\":%d,\"hz\":%d}]}",
              poll_slot, request, value, length, hz);
}

static void the_poll_budget_is_shared_by_every_session(void)
{
    start();
    open_session(0);
    open_session(1);
    attach_device();

    /* One bulk-sized poll takes most of the 40,000 bytes a second. */
    subscribe_one(0, 0, 0xA0, 0, 8192, 20);
    cJSON *ok = last_json(0, "ok");
    const cJSON *grant = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(ok, "granted"), 0);
    const int granted = json_number(grant, "hz");
    CHECK(granted >= 1 && granted < 20);
    CHECK((uint32_t)granted * 8192 <= hub_poll_budget(&hub));
    cJSON_Delete(ok);

    /* Another session cannot add a second one on top, even at 1 Hz... */
    subscribe_one(1, 0, 0xA0, 1, 8192, 1);
    cJSON *err = last_json(1, "err");
    CHECK(strcmp(json_string(err, "code"), "busy") == 0);
    cJSON_Delete(err);
    CHECK(!hub.sessions[1].polls[0].active);

    /* ...but may share the one that runs, which costs nothing more. */
    forget_messages();
    subscribe_one(1, 0, 0xA0, 0, 8192, 1);
    ok = last_json(1, "ok");
    grant = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(ok, "granted"), 0);
    CHECK(json_number(grant, "hz") == 1);
    cJSON_Delete(ok);
}

/* True when `length` bytes are well-formed UTF-8. */
static bool valid_utf8(const uint8_t *text, size_t length)
{
    for (size_t i = 0; i < length;) {
        const uint8_t lead = text[i];
        const size_t size = lead < 0x80           ? 1
                            : (lead >> 5) == 0x6  ? 2
                            : (lead >> 4) == 0xE  ? 3
                            : (lead >> 3) == 0x1E ? 4
                                                  : 0;
        if (size == 0 || i + size > length) {
            return false;
        }
        for (size_t k = 1; k < size; k++) {
            if ((text[i + k] & 0xC0) != 0x80) {
                return false;
            }
        }
        i += size;
    }
    return true;
}

static void client_names_are_kept_valid_utf8(void)
{
    start();
    hub_set_pin(&hub, "482913");
    hub_session_opened(&hub, 0, CLIENT_ADDRESS);
    /* 64 bytes: a cut at 63 would split the last character. */
    char name[128] = "a";
    for (int i = 0; i < 21; i++) {
        strcat(name, "\xE6\x97\xA5");
    }
    send_json(0,
              "{\"t\":\"hello\",\"proto\":{\"major\":1,\"minor\":2},\"client\":{\"name\":\"%s\"}}",
              name);
    send_json(0, "{\"t\":\"auth.pair\",\"id\":2,\"pin\":\"482913\"}");
    forget_messages();
    send_json(0, "{\"t\":\"auth.list\",\"id\":3}");
    CHECK(io_record.message_count == 1);
    CHECK(valid_utf8(io_record.messages[0].data, io_record.messages[0].length));
    cJSON *ok = last_json(0, "ok");
    const cJSON *client = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(ok, "clients"), 0);
    CHECK(strlen(json_string(client, "name")) == 61); /* "a" and 20 whole characters */
    cJSON_Delete(ok);

    /* Bytes that are not UTF-8 at all become '?'. */
    hub_session_opened(&hub, 1, CLIENT_ADDRESS);
    send_json(1, "{\"t\":\"hello\",\"proto\":{\"major\":1,\"minor\":2},\"client\":{\"name\":"
                 "\"x\xFF\xC0\"}}");
    CHECK(strcmp(hub.sessions[1].client_name, "x??") == 0);
}

static void an_unreadable_pin_lets_nobody_in_until_it_is_set_again(void)
{
    start_with(true);
    hub_session_opened(&hub, 0, CLIENT_ADDRESS);
    send_json(0, "{\"t\":\"hello\",\"proto\":{\"major\":1,\"minor\":2}}");
    cJSON *hello = last_json(0, "hello");
    CHECK(strcmp(json_string(hello, "auth"), "pin") == 0);
    CHECK(strcmp(json_string(hello, "pin_source"), "fixed") == 0);
    cJSON_Delete(hello);

    send_json(0, "{\"t\":\"auth.pair\",\"id\":1,\"pin\":\"000000\"}");
    cJSON *err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "code"), "internal") == 0);
    cJSON_Delete(err);
    forget_messages();
    send_json(0, "{\"t\":\"auth.token\",\"id\":2,\"token\":\"%043d\"}", 0);
    err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "code"), "internal") == 0);
    cJSON_Delete(err);
    CHECK(hub.sessions[0].state == HUB_SESSION_AUTHENTICATING);
    CHECK(hub_check_pin(&hub, CLIENT_ADDRESS, "000000") == HUB_AUTH_UNAVAILABLE);
    CHECK(hub_pin_enabled(&hub));

    /* Setting the PIN from the setup page recovers. */
    CHECK(hub_set_pin(&hub, "482913") == HUB_AUTH_OK);
    CHECK(hub_check_pin(&hub, CLIENT_ADDRESS, "482913") == HUB_AUTH_OK);
    start_with(true);
    CHECK(hub_set_pin(&hub, NULL) == HUB_AUTH_OK);
    CHECK(!hub_pin_enabled(&hub));
}

static void one_address_cannot_fill_the_bridge_unauthenticated(void)
{
    start();
    hub_set_pin(&hub, "482913");
    hello_only(0);
    io_record.now_ms += 10;
    hello_only(1);
    io_record.now_ms += 10;
    CHECK(io_record.close_code[0] == 0 && io_record.close_code[1] == 0);
    hello_only(2); /* A third from the same address closes its oldest */
    CHECK(io_record.close_code[0] == 4001);
    CHECK(io_record.close_code[1] == 0 && io_record.close_code[2] == 0);
    hub_session_opened(&hub, 3, CLIENT_ADDRESS + 1); /* Others are not affected */
    CHECK(io_record.close_code[3] == 0);

    /* With auth none every session is in at hello, so nothing is limited. */
    start();
    for (int slot = 0; slot < 3; slot++) {
        open_session(slot);
    }
    CHECK(io_record.close_code[0] == 0);
}

/* Pairs session `slot` with the PIN; returns its client id. */
static uint16_t pair_session(int slot)
{
    hello_only(slot);
    send_json(slot, "{\"t\":\"auth.pair\",\"id\":1,\"pin\":\"482913\",\"name\":\"S%d\"}", slot);
    return hub.sessions[slot].cid;
}

/* Pairs clients straight through hub_auth until the list is full. */
static void fill_client_list(void)
{
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    uint16_t cid = 0;
    size_t count = 0;
    const hub_auth_client_t *clients = hub_auth_clients(&hub.auth, &count);
    for (size_t i = 0; i < count; i++) {
        if (!clients[i].used) {
            CHECK(hub_auth_issue(&hub.auth, "Other", token, &cid, NULL) == HUB_AUTH_OK);
        }
    }
}

static void a_client_forgotten_for_room_is_disconnected(void)
{
    start();
    hub_set_pin(&hub, "482913");
    pair_session(0); /* The first in the list, and so the stalest */
    unix_now += 10;
    fill_client_list();
    unix_now += 10;
    pair_session(1);
    CHECK(io_record.close_code[0] == 4002); /* Its token is gone */
    CHECK(io_record.close_code[1] == 0);
}

static void a_connected_client_stays_seen(void)
{
    start();
    hub_set_pin(&hub, "482913");
    pair_session(0);
    unix_now += 10;
    fill_client_list();
    /* Session 0 stays connected while time passes. */
    for (int i = 0; i < 3; i++) {
        io_record.now_ms += HUB_AUTH_TOUCH_INTERVAL_MS;
        unix_now += 60;
        hub_session_activity(&hub, 0);
        hub_tick(&hub);
    }
    pair_session(1); /* Forgets a client that is away instead */
    CHECK(io_record.close_code[0] == 0);
    CHECK(hub.sessions[0].state == HUB_SESSION_OPEN);
}

static void viewers_cannot_administer_or_rename(void)
{
    start();
    open_session(0);
    attach_device();
    hub.sessions[0].role = LINK_ROLE_VIEWER; /* A hub with roles would grant this */
    const char *requests[] = {
        "{\"t\":\"hub.stats\",\"id\":1}",
        "{\"t\":\"hub.rename\",\"id\":1,\"name\":\"Studio\"}",
        "{\"t\":\"device.rename\",\"id\":1,\"handle\":0,\"name\":\"Studio\"}",
    };
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
        forget_messages();
        send_json(0, "%s", requests[i]);
        cJSON *err = last_json(0, "err");
        CHECK(strcmp(json_string(err, "code"), "denied") == 0);
        cJSON_Delete(err);
    }
    CHECK(io_record.apply_attempts == 0);
    CHECK(io_record.saved_device_name[0] == '\0');
}

static void uptime_survives_the_clock_wrapping(void)
{
    start();
    open_session(0);
    for (int hour = 0; hour < 50 * 24; hour++) {
        io_record.now_ms += 60u * 60u * 1000u;
        hub_tick(&hub);
    }
    open_session(1); /* Session 0 timed out long ago */
    send_json(1, "{\"t\":\"hub.stats\",\"id\":1}");
    cJSON *ok = last_json(1, "ok");
    CHECK(json_number(ok, "uptime_s") == 50 * 24 * 3600);
    cJSON_Delete(ok);
}

static void a_failed_change_puts_the_old_settings_back(void)
{
    start();
    open_session(0);
    const uint32_t old_baud = hub.config.settings.uart_baud;
    io_record.refuse_apply = true;
    send_json(0, "{\"t\":\"hub.config.set\",\"id\":2,\"config\":{\"uart\":{\"baud\":921600}}}");
    cJSON *err = last_json(0, "err");
    CHECK(strcmp(json_string(err, "code"), "internal") == 0);
    cJSON_Delete(err);
    /* The I/O may have saved part of it: the old settings go back. */
    CHECK(io_record.apply_attempts == 2);
    CHECK(io_record.attempted.uart_baud == old_baud);
    CHECK(hub.config.settings.uart_baud == old_baud);
}

void test_hub(void)
{
    RUN(hello_describes_the_bridge);
    RUN(the_first_message_must_be_hello);
    RUN(deeply_nested_json_is_refused);
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
    RUN(snapshots_read_the_status_fresh_and_the_blob_when_it_changed);
    RUN(a_snapshot_asked_during_a_status_read_waits_for_the_next);
    RUN(without_notifications_every_snapshot_reads_the_blob);
    RUN(write_as_read_commands_mark_the_cache_stale);
    RUN(an_answer_for_an_abandoned_snapshot_read_is_ignored);
    RUN(a_lock_keeps_other_sessions_out);
    RUN(the_hub_can_be_renamed);
    RUN(with_the_pin_on_hello_asks_for_it);
    RUN(pairing_with_the_pin_issues_a_token);
    RUN(setting_the_pin_keeps_the_setter_and_signs_out_the_rest);
    RUN(a_revoked_client_is_disconnected);
    RUN(config_get_shows_settings_and_choices);
    RUN(config_set_applies_whole_changes_only);
    RUN(a_detected_rate_is_kept_and_shown);
    RUN(wifi_scans_answer_later);
    RUN(joining_another_network_closes_every_session);
    RUN(a_factory_reset_tells_everyone_then_restarts);
    RUN(an_update_is_written_chunk_by_chunk_then_restarts);
    RUN(bad_updates_are_refused_and_keep_the_firmware);
    RUN(a_stalled_or_orphaned_update_is_abandoned);
    RUN(an_update_holds_back_large_answers_until_it_ends);
    RUN(a_complete_update_survives_its_sender_leaving);
    RUN(reports_from_an_abandoned_update_are_ignored);
    RUN(a_lost_answer_times_out_and_a_late_one_is_ignored);
    RUN(an_unknown_answer_finishes_only_older_requests);
    RUN(a_refused_subscription_changes_nothing);
    RUN(repeated_keys_need_one_group);
    RUN(a_failing_poll_reports_once);
    RUN(one_session_cannot_fill_the_snapshot_queue);
    RUN(failures_the_io_reports_are_errors);
    RUN(a_failed_hash_keeps_the_pin_off);
    RUN(a_refused_write_claims_no_notification);
    RUN(write_as_read_commands_are_attributed);
    RUN(a_write_queued_behind_a_slow_one_keeps_its_window);
    RUN(the_poll_budget_is_shared_by_every_session);
    RUN(client_names_are_kept_valid_utf8);
    RUN(an_unreadable_pin_lets_nobody_in_until_it_is_set_again);
    RUN(one_address_cannot_fill_the_bridge_unauthenticated);
    RUN(a_client_forgotten_for_room_is_disconnected);
    RUN(a_connected_client_stays_seen);
    RUN(viewers_cannot_administer_or_rename);
    RUN(uptime_survives_the_clock_wrapping);
    RUN(a_failed_change_puts_the_old_settings_back);
}
