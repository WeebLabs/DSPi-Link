/*
 * hub.c - sessions, the message dispatch table, hello and keepalive.
 *
 * Implements Link spec 4 (keepalive), 5 (session lifecycle), 6.1 (`auth:
 * none`), 7.1 (the JSON envelope), 7.2 (hello) and 7.9 (hub management), and
 * bridge spec 8.1 and 8.2. See hub_internal.h for how the other hub files
 * divide the rest.
 *
 * Portable: no ESP-IDF headers.
 */

#include <stdio.h>
#include <string.h>

#include "hub_internal.h"

/* WebSocket close codes the hub sends (Link spec 5). */
#define CLOSE_KEEPALIVE_FAILURE 1001
#define CLOSE_PROTOCOL_ERROR 4000
#define CLOSE_AUTH_TIMEOUT 4001

/* The protocol version this hub speaks (Link spec 10). */
#define PROTOCOL_MAJOR 1
#define PROTOCOL_MINOR 2

/* An `id` of -1 means the request carried none, so replies leave it out. */
#define NO_ID (-1)

/* ---------------------------------------------------------------------------
 * Helpers shared with the other hub files
 * ------------------------------------------------------------------------- */

uint32_t hub_now(const hub_t *hub)
{
    return hub->io.now_ms(hub->io.context);
}

static bool valid_slot(const hub_t *hub, int slot)
{
    return slot >= 0 && (size_t)slot < hub->config.max_sessions;
}

hub_session_t *hub_open_session(hub_t *hub, int slot)
{
    if (!valid_slot(hub, slot) || hub->sessions[slot].state != HUB_SESSION_OPEN) {
        return NULL;
    }
    return &hub->sessions[slot];
}

bool hub_send_json(hub_t *hub, int slot, cJSON *message)
{
    char *text = cJSON_PrintUnformatted(message);
    cJSON_Delete(message);
    if (text == NULL) {
        return false;
    }
    const bool sent = hub->io.send_text(hub->io.context, slot, text, strlen(text));
    cJSON_free(text);
    return sent;
}

void hub_broadcast_json(hub_t *hub, cJSON *message)
{
    /* Printed once, sent to everyone. */
    char *text = cJSON_PrintUnformatted(message);
    cJSON_Delete(message);
    if (text == NULL) {
        return;
    }
    const size_t length = strlen(text);
    for (size_t slot = 0; slot < hub->config.max_sessions; slot++) {
        if (hub->sessions[slot].state == HUB_SESSION_OPEN) {
            hub->io.send_text(hub->io.context, (int)slot, text, length);
        }
    }
    cJSON_free(text);
}

bool hub_send_binary(hub_t *hub, int slot, const uint8_t *data, size_t length)
{
    return hub->io.send_binary(hub->io.context, slot, data, length);
}

/* Starts a reply envelope: {"t": type, "id": id}. */
static cJSON *new_reply(const char *type, int id)
{
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "t", type);
    if (id != NO_ID) {
        cJSON_AddNumberToObject(reply, "id", id);
    }
    return reply;
}

void hub_reply_ok(hub_t *hub, int slot, int id, cJSON *body)
{
    cJSON *reply = new_reply("ok", id);
    if (body != NULL) {
        /* Move the body's members into the envelope. */
        cJSON *member = body->child;
        while (member != NULL) {
            cJSON *next = member->next;
            cJSON_DetachItemViaPointer(body, member);
            cJSON_AddItemToObject(reply, member->string, member);
            member = next;
        }
        cJSON_Delete(body);
    }
    hub_send_json(hub, slot, reply);
}

void hub_reply_error(hub_t *hub, int slot, int id, const char *code, const char *msg)
{
    cJSON *reply = new_reply("err", id);
    cJSON_AddStringToObject(reply, "code", code);
    if (msg != NULL) {
        cJSON_AddStringToObject(reply, "msg", msg);
    }
    hub_send_json(hub, slot, reply);
}

bool hub_json_int(const cJSON *object, const char *name, int *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *value = item->valueint;
    return true;
}

/* ---------------------------------------------------------------------------
 * Sessions
 * ------------------------------------------------------------------------- */

void hub_init(hub_t *hub, const hub_io_t *io, const hub_config_t *config)
{
    memset(hub, 0, sizeof(*hub));
    hub->io = *io;
    hub->config = *config;
    if (hub->config.max_sessions > HUB_MAX_SESSIONS) {
        hub->config.max_sessions = HUB_MAX_SESSIONS;
    }
    hub->started_ms = hub_now(hub);
    hub->next_session_id = 1;
    hub->next_tag = 1;
    hub->device.state = HUB_DEVICE_ABSENT;
}

bool hub_has_room(const hub_t *hub)
{
    for (size_t slot = 0; slot < hub->config.max_sessions; slot++) {
        if (hub->sessions[slot].state == HUB_SESSION_FREE) {
            return true;
        }
    }
    return false;
}

void hub_session_opened(hub_t *hub, int slot)
{
    if (!valid_slot(hub, slot)) {
        return;
    }
    hub_session_t *session = &hub->sessions[slot];
    memset(session, 0, sizeof(*session));
    session->state = HUB_SESSION_AWAITING_HELLO;
    session->opened_ms = hub_now(hub);
    session->last_activity_ms = session->opened_ms;
    session->last_ping_ms = session->opened_ms;
}

/* Releases everything a session held: its polls, its lock and any snapshot
 * it waits for. Safe to call twice. */
static void release_session(hub_t *hub, int slot)
{
    const uint16_t session_id = hub->sessions[slot].id;
    hub_polls_session_closed(hub, slot);
    hub_device_session_closed(hub, slot, session_id);
}

/* Asks the network side to close a session, and stops serving it now. */
static void close_session(hub_t *hub, int slot, uint16_t code)
{
    release_session(hub, slot);
    hub->sessions[slot].state = HUB_SESSION_CLOSING;
    hub->io.close(hub->io.context, slot, code);
}

void hub_session_closed(hub_t *hub, int slot)
{
    if (!valid_slot(hub, slot) || hub->sessions[slot].state == HUB_SESSION_FREE) {
        return;
    }
    release_session(hub, slot);
    hub->sessions[slot].state = HUB_SESSION_FREE;
}

void hub_session_activity(hub_t *hub, int slot)
{
    if (valid_slot(hub, slot) && hub->sessions[slot].state != HUB_SESSION_FREE) {
        hub->sessions[slot].last_activity_ms = hub_now(hub);
    }
}

/* A session id not in use, never 0 (Link spec 7.3). */
static uint16_t allocate_session_id(hub_t *hub)
{
    for (;;) {
        const uint16_t candidate = hub->next_session_id++;
        if (candidate == 0) {
            continue;
        }
        bool in_use = false;
        for (size_t slot = 0; slot < hub->config.max_sessions; slot++) {
            in_use = in_use || (hub->sessions[slot].state != HUB_SESSION_FREE &&
                                hub->sessions[slot].id == candidate);
        }
        if (!in_use) {
            return candidate;
        }
    }
}

/* ---------------------------------------------------------------------------
 * hello (Link spec 7.2) and auth in `none` mode (Link spec 6.1)
 * ------------------------------------------------------------------------- */

uint32_t hub_poll_budget(const hub_t *hub)
{
    const uint32_t baud =
        hub->device.state == HUB_DEVICE_ABSENT ? DSPI_UART_DEFAULT_BAUD : hub->device.baud;
    return baud / LINK_POLL_BUDGET_BAUD_DIVISOR;
}

static cJSON *new_proto_object(void)
{
    cJSON *proto = cJSON_CreateObject();
    cJSON_AddNumberToObject(proto, "major", PROTOCOL_MAJOR);
    cJSON_AddNumberToObject(proto, "minor", PROTOCOL_MINOR);
    return proto;
}

static cJSON *new_hub_object(const hub_t *hub)
{
    cJSON *object = cJSON_CreateObject();
    cJSON_AddStringToObject(object, "id", hub->config.hub_id);
    cJSON_AddStringToObject(object, "name", hub->config.name);
    cJSON_AddStringToObject(object, "kind", "bridge");
    cJSON_AddStringToObject(object, "version", hub->config.version);
    return object;
}

/* The capabilities of bridge spec 8.1 that phase 3 implements. `notify` is
 * left out when the DSPi has notifications turned off (bridge spec 5). */
static cJSON *new_caps_array(const hub_t *hub)
{
    const bool notify_off = hub->device.state != HUB_DEVICE_ABSENT &&
                            hub->device.identity.has_uart_config &&
                            !hub->device.identity.notify_enabled;
    cJSON *caps = cJSON_CreateArray();
    cJSON_AddItemToArray(caps, cJSON_CreateString("cmd"));
    if (!notify_off) {
        cJSON_AddItemToArray(caps, cJSON_CreateString("notify"));
    }
    cJSON_AddItemToArray(caps, cJSON_CreateString("poll"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("snapshot"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("lock"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("web"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("rename"));
    return caps;
}

static void handle_hello(hub_t *hub, int slot, const cJSON *message)
{
    hub_session_t *session = &hub->sessions[slot];

    int major = 0;
    const cJSON *proto = cJSON_GetObjectItemCaseSensitive(message, "proto");
    if (!hub_json_int(proto, "major", &major) || major != PROTOCOL_MAJOR) {
        close_session(hub, slot, CLOSE_PROTOCOL_ERROR);
        return;
    }

    const cJSON *client = cJSON_GetObjectItemCaseSensitive(message, "client");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(client, "name");
    if (cJSON_IsString(name)) {
        snprintf(session->client_name, sizeof(session->client_name), "%s", name->valuestring);
    }

    /* With `auth: none` the session opens as soon as the hub answers, and
     * every session is admin (Link spec 6.1). */
    session->id = allocate_session_id(hub);
    session->role = LINK_ROLE_ADMIN;
    session->state = HUB_SESSION_OPEN;

    cJSON *limits = cJSON_CreateObject();
    cJSON_AddNumberToObject(limits, "max_frame", LINK_MAX_FRAME_BYTES);
    cJSON_AddNumberToObject(limits, "max_payload", LINK_MAX_PAYLOAD_BYTES);
    cJSON_AddNumberToObject(limits, "max_inflight", LINK_MAX_INFLIGHT);
    cJSON_AddNumberToObject(limits, "poll_max_hz", LINK_POLL_MAX_HZ);
    cJSON_AddNumberToObject(limits, "poll_budget_bps", hub_poll_budget(hub));

    cJSON *hello = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "t", "hello");
    cJSON_AddItemToObject(hello, "proto", new_proto_object());
    cJSON_AddItemToObject(hello, "hub", new_hub_object(hub));
    cJSON_AddStringToObject(hello, "auth", "none");
    cJSON_AddItemToObject(hello, "caps", new_caps_array(hub));
    cJSON_AddItemToObject(hello, "limits", limits);
    hub_send_json(hub, slot, hello);
}

/* auth.pair and auth.token. With `auth: none` a client may still pair, with
 * any PIN, to learn its session id for echo suppression (Link spec 6.1). */
static void handle_auth(hub_t *hub, int slot, int id, const cJSON *message)
{
    hub_session_t *session = &hub->sessions[slot];
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(message, "name");
    if (cJSON_IsString(name)) {
        snprintf(session->client_name, sizeof(session->client_name), "%s", name->valuestring);
    }

    cJSON *policy = cJSON_CreateObject();
    cJSON_AddItemToObject(policy, "denied", cJSON_CreateArray()); /* Admin is denied nothing */

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "session", session->id);
    cJSON_AddStringToObject(body, "role", link_policy_role_name(session->role));
    cJSON_AddItemToObject(body, "policy", policy);
    hub_reply_ok(hub, slot, id, body);
}

/* ---------------------------------------------------------------------------
 * Hub management (Link spec 7.9)
 * ------------------------------------------------------------------------- */

static void handle_hub_stats(hub_t *hub, int slot, int id, const cJSON *message)
{
    (void)message;
    unsigned open_sessions = 0;
    for (size_t i = 0; i < hub->config.max_sessions; i++) {
        open_sessions += hub->sessions[i].state == HUB_SESSION_OPEN ? 1 : 0;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "sessions", open_sessions);
    cJSON_AddNumberToObject(body, "uptime_s", (hub_now(hub) - hub->started_ms) / 1000);

    cJSON *devices = cJSON_AddArrayToObject(body, "devices");
    if (hub->device.state != HUB_DEVICE_ABSENT) {
        cJSON *device = cJSON_CreateObject();
        cJSON_AddNumberToObject(device, "handle", HUB_DEVICE_HANDLE);
        cJSON_AddNumberToObject(device, "cmds", hub->device.commands);
        cJSON_AddNumberToObject(device, "errors", hub->device.errors);
        cJSON_AddNumberToObject(device, "avg_rtt_ms", hub->device.average_rtt_ms);
        cJSON_AddNumberToObject(device, "notify_dropped", hub->device.notifications_dropped);
        cJSON_AddItemToArray(devices, device);
    }

    if (hub->io.describe_bridge != NULL) {
        hub->io.describe_bridge(hub->io.context, cJSON_AddObjectToObject(body, "bridge"));
    }
    hub_reply_ok(hub, slot, id, body);
}

bool hub_rename(hub_t *hub, const char *name)
{
    if (!settings_name_is_valid(name)) {
        return false;
    }
    snprintf(hub->config.name, sizeof(hub->config.name), "%s", name);
    hub->io.save_hub_name(hub->io.context, hub->config.name);
    hub->io.advertise(hub->io.context);
    return true;
}

static void handle_hub_rename(hub_t *hub, int slot, int id, const cJSON *message)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(message, "name");
    if (!cJSON_IsString(name) || !hub_rename(hub, name->valuestring)) {
        hub_reply_error(hub, slot, id, "bad_request", "name must be 1 to 63 bytes of UTF-8");
        return;
    }
    hub_reply_ok(hub, slot, id, NULL);
}

/* ---------------------------------------------------------------------------
 * Incoming messages
 * ------------------------------------------------------------------------- */

typedef void (*message_handler_t)(hub_t *hub, int slot, int id, const cJSON *message);

/* Every request type the hub answers. Anything else gets `unknown_type`
 * (Link spec 7.1). */
static const struct {
    const char *type;
    message_handler_t handle;
} MESSAGE_HANDLERS[] = {
    {"auth.pair", handle_auth},
    {"auth.token", handle_auth},
    {"device.list", hub_handle_device_list},
    {"device.rename", hub_handle_device_rename},
    {"device.snapshot", hub_handle_device_snapshot},
    {"poll.subscribe", hub_handle_poll_subscribe},
    {"poll.unsubscribe", hub_handle_poll_unsubscribe},
    {"lock.acquire", hub_handle_lock_acquire},
    {"lock.release", hub_handle_lock_release},
    {"hub.stats", handle_hub_stats},
    {"hub.rename", handle_hub_rename},
};

static void dispatch_request(hub_t *hub, int slot, const char *type, int id, const cJSON *message)
{
    for (size_t i = 0; i < sizeof(MESSAGE_HANDLERS) / sizeof(MESSAGE_HANDLERS[0]); i++) {
        if (strcmp(type, MESSAGE_HANDLERS[i].type) == 0) {
            MESSAGE_HANDLERS[i].handle(hub, slot, id, message);
            return;
        }
    }
    hub_reply_error(hub, slot, id, "unknown_type", NULL);
}

void hub_session_text(hub_t *hub, int slot, const char *text, size_t length)
{
    if (!valid_slot(hub, slot)) {
        return;
    }
    hub_session_t *session = &hub->sessions[slot];
    if (session->state != HUB_SESSION_AWAITING_HELLO && session->state != HUB_SESSION_OPEN) {
        return;
    }
    session->last_activity_ms = hub_now(hub);

    cJSON *message = cJSON_ParseWithLength(text, length);
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(message, "t");
    const bool well_formed = cJSON_IsObject(message) && cJSON_IsString(type);

    if (session->state == HUB_SESSION_AWAITING_HELLO) {
        /* The first message must be hello (Link spec 5). */
        if (well_formed && strcmp(type->valuestring, "hello") == 0) {
            handle_hello(hub, slot, message);
        } else {
            close_session(hub, slot, CLOSE_PROTOCOL_ERROR);
        }
    } else if (!well_formed) {
        hub_reply_error(hub, slot, NO_ID, "bad_request", "not a JSON object with a string t");
    } else {
        int id = NO_ID;
        hub_json_int(message, "id", &id);
        if (strcmp(type->valuestring, "hello") == 0) {
            hub_reply_error(hub, slot, id, "bad_request", "hello was already exchanged");
        } else {
            dispatch_request(hub, slot, type->valuestring, id, message);
        }
    }
    cJSON_Delete(message);
}

void hub_session_binary(hub_t *hub, int slot, const uint8_t *data, size_t length)
{
    hub_session_activity(hub, slot);
    if (hub_open_session(hub, slot) == NULL) {
        return; /* Binary frames before hello are discarded (Link spec 5) */
    }

    const int type = link_frame_type(data, length);
    if (type == LINK_FRAME_CMD_REQUEST) {
        hub_commands_handle_frame(hub, slot, data, length);
    } else if (type == LINK_FRAME_FWDATA) {
        /* No transfer is open: hub_update arrives in phase 6. Refuse the chunk. */
        link_fwdata_chunk_t chunk;
        if (link_decode_fwdata(data, length, &chunk)) {
            const size_t frame_length = link_encode_fwdata_ack(hub->frame, sizeof(hub->frame),
                                                               chunk.transfer, chunk.offset, 1);
            hub_send_binary(hub, slot, hub->frame, frame_length);
        }
    } else if (type >= 0 && type < 0x80) {
        /* An unknown request type gets UNSUPPORTED with its tag (Link spec 8). */
        const size_t frame_length =
            link_encode_cmd_response(hub->frame, sizeof(hub->frame), link_frame_tag(data, length),
                                     LINK_STATUS_UNSUPPORTED, NULL, 0);
        hub_send_binary(hub, slot, hub->frame, frame_length);
    }
    /* Types from 0x80 are hub-to-client; a hub ignores them. */
}

/* ---------------------------------------------------------------------------
 * Timers
 * ------------------------------------------------------------------------- */

/* Hello timeout, keepalive and owed RESYNCs (Link spec 4 and 5, bridge spec
 * 8.2). */
static void tick_sessions(hub_t *hub, uint32_t now)
{
    for (size_t i = 0; i < hub->config.max_sessions; i++) {
        hub_session_t *session = &hub->sessions[i];
        const int slot = (int)i;

        if (session->state == HUB_SESSION_AWAITING_HELLO &&
            now - session->opened_ms >= LINK_AUTH_TIMEOUT_MS) {
            close_session(hub, slot, CLOSE_AUTH_TIMEOUT);
            continue;
        }
        if (session->state != HUB_SESSION_OPEN) {
            continue;
        }
        if (now - session->last_activity_ms >= LINK_PING_TIMEOUT_MS) {
            close_session(hub, slot, CLOSE_KEEPALIVE_FAILURE);
            continue;
        }
        if (now - session->last_ping_ms >= LINK_PING_INTERVAL_MS) {
            hub->io.ping(hub->io.context, slot);
            session->last_ping_ms = now;
        }
        if (session->resync_owed) {
            const size_t length = link_encode_resync(hub->frame, sizeof(hub->frame),
                                                     HUB_DEVICE_HANDLE, LINK_RESYNC_DROPPED);
            session->resync_owed = !hub_send_binary(hub, slot, hub->frame, length);
        }
    }
}

void hub_tick(hub_t *hub)
{
    tick_sessions(hub, hub_now(hub));
    hub_device_tick(hub);
    hub_polls_tick(hub);
}

/* ---------------------------------------------------------------------------
 * What the hub publishes (Link spec 3)
 * ------------------------------------------------------------------------- */

char *hub_info_json(const hub_t *hub)
{
    cJSON *info = cJSON_CreateObject();
    cJSON_AddItemToObject(info, "proto", new_proto_object());
    cJSON_AddItemToObject(info, "hub", new_hub_object(hub));
    cJSON_AddStringToObject(info, "auth", "none");
    cJSON_AddBoolToObject(info, "tls", false);
    cJSON_AddStringToObject(info, "ws", "/dspi/v1");

    cJSON *devices = cJSON_AddArrayToObject(info, "devices");
    if (hub->device.state != HUB_DEVICE_ABSENT) {
        cJSON *device = cJSON_CreateObject();
        cJSON_AddStringToObject(device, "serial", hub->device.identity.serial);
        cJSON_AddStringToObject(device, "name", hub->device.name);
        cJSON_AddItemToArray(devices, device);
    }

    char *text = cJSON_PrintUnformatted(info);
    cJSON_Delete(info);
    return text;
}

void hub_advert(const hub_t *hub, hub_advert_t *advert)
{
    const bool known = hub->device.state != HUB_DEVICE_ABSENT;
    advert->hub_id = hub->config.hub_id;
    advert->name = hub->config.name;
    advert->auth = "none";
    advert->devices = known ? 1 : 0;
    advert->serial = known ? hub->device.identity.serial : NULL;
}
