/*
 * hub_admin.c - administering the hub: its settings, its Wi-Fi, its PIN and
 * paired clients, and restarting or resetting it.
 *
 * Implements Link spec 7.3 (auth.list, auth.revoke, auth.set_role) and 7.10
 * to 7.13, as bridge spec 8.9 to 8.11 describe. Every message here requires
 * admin; on the bridge every authenticated session is admin.
 *
 * Portable: no ESP-IDF headers.
 */

#include <stdio.h>
#include <string.h>

#include "hub_config.h"
#include "hub_internal.h"

static bool require_admin(hub_t *hub, int slot, int id)
{
    if (hub->sessions[slot].role != LINK_ROLE_ADMIN) {
        hub_reply_error(hub, slot, id, "denied", "admin role required");
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Settings (Link spec 7.10)
 * ------------------------------------------------------------------------- */

void hub_apply_settings(hub_t *hub, const bridge_settings_t *settings)
{
    hub->config.settings = *settings;
    hub->io.apply_settings(hub->io.context, settings);
    hub->io.advertise(hub->io.context);

    cJSON *event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "t", "hub.config.changed");
    cJSON_AddItemToObject(event, "config", hub_config_to_json(settings));
    hub_broadcast_json(hub, event);
}

void hub_handle_config_get(hub_t *hub, int slot, int id, const cJSON *message)
{
    (void)message;
    if (!require_admin(hub, slot, id)) {
        return;
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "config", hub_config_to_json(&hub->config.settings));
    cJSON_AddItemToObject(body, "options", hub_config_options(hub->config.board));
    hub_reply_ok(hub, slot, id, body);
}

void hub_handle_config_set(hub_t *hub, int slot, int id, const cJSON *message)
{
    if (!require_admin(hub, slot, id)) {
        return;
    }
    bridge_settings_t updated;
    const char *problem = hub_config_apply(cJSON_GetObjectItemCaseSensitive(message, "config"),
                                           &hub->config.settings, hub->config.board, &updated);
    if (problem != NULL) {
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "t", "err");
        if (id >= 0) {
            cJSON_AddNumberToObject(reply, "id", id);
        }
        cJSON_AddStringToObject(reply, "code", "bad_request");
        cJSON_AddStringToObject(reply, "msg", "a setting is unknown or not allowed");
        cJSON_AddStringToObject(reply, "field", problem); /* Link spec 7.10 */
        hub_send_json(hub, slot, reply);
        return;
    }

    /* Reply before applying, so the reply leads hub.config.changed. */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "config", hub_config_to_json(&updated));
    hub_reply_ok(hub, slot, id, body);
    hub_apply_settings(hub, &updated);
}

/* ---------------------------------------------------------------------------
 * Wi-Fi (Link spec 7.11)
 * ------------------------------------------------------------------------- */

void hub_handle_wifi_scan(hub_t *hub, int slot, int id, const cJSON *message)
{
    (void)message;
    if (!require_admin(hub, slot, id)) {
        return;
    }
    /* A scan takes seconds; the I/O runs it in the background and answers
     * through hub_reply_later(). */
    hub->io.start_wifi_scan(hub->io.context, slot, hub->sessions[slot].id, id);
}

void hub_handle_wifi_join(hub_t *hub, int slot, int id, const cJSON *message)
{
    if (!require_admin(hub, slot, id)) {
        return;
    }
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(message, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(message, "password");
    if (!cJSON_IsString(ssid) || !cJSON_IsString(password) ||
        !hub->io.wifi_join(hub->io.context, ssid->valuestring, password->valuestring)) {
        hub_reply_error(hub, slot, id, "bad_request",
                        "ssid must be 1 to 32 bytes and password empty or 8 to 63 characters");
        return;
    }
    /* `ok` means accepted. Every session closes while the bridge moves;
     * clients find it again by its hub id (Link spec 7.11). */
    hub_reply_ok(hub, slot, id, NULL);
    hub_close_sessions(hub, CLOSE_SHUTTING_DOWN, -1);
}

/* ---------------------------------------------------------------------------
 * The PIN and paired clients (Link spec 7.3 and 7.12)
 * ------------------------------------------------------------------------- */

/* Room for an ISO-8601 time; generous, because the compiler cannot see that
 * the year has four digits. */
#define TIME_TEXT_BYTES 32

/* Writes `unix_time` as ISO-8601 UTC ("2026-09-08T10:12:00Z") using the
 * civil-from-days algorithm, which needs no time library. */
static void format_time(uint32_t unix_time, char out[TIME_TEXT_BYTES])
{
    const int64_t days = unix_time / 86400;
    const uint32_t seconds = unix_time % 86400;
    const int64_t shifted = days + 719468;
    const int64_t era = shifted / 146097;
    const int64_t day_of_era = shifted - era * 146097;
    const int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const int64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const int64_t month_index = (5 * day_of_year + 2) / 153;
    const int day = (int)(day_of_year - (153 * month_index + 2) / 5 + 1);
    const int month = (int)(month_index < 10 ? month_index + 3 : month_index - 9);
    const int year = (int)(year_of_era + era * 400 + (month <= 2 ? 1 : 0));
    snprintf(out, TIME_TEXT_BYTES, "%04d-%02d-%02dT%02u:%02u:%02uZ", year, month, day,
             (unsigned)(seconds / 3600), (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60));
}

static bool client_online(const hub_t *hub, uint16_t cid)
{
    for (size_t slot = 0; slot < hub->config.max_sessions; slot++) {
        if (hub->sessions[slot].state == HUB_SESSION_OPEN && hub->sessions[slot].cid == cid) {
            return true;
        }
    }
    return false;
}

void hub_handle_auth_list(hub_t *hub, int slot, int id, const cJSON *message)
{
    (void)message;
    if (!require_admin(hub, slot, id)) {
        return;
    }
    size_t count = 0;
    const hub_auth_client_t *clients = hub_auth_clients(&hub->auth, &count);
    cJSON *body = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(body, "clients");
    for (size_t i = 0; i < count; i++) {
        if (!clients[i].used) {
            continue;
        }
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "cid", clients[i].cid);
        cJSON_AddStringToObject(entry, "name", clients[i].name);
        cJSON_AddStringToObject(entry, "role", "admin");
        char when[TIME_TEXT_BYTES];
        /* A time taken before the clock was set is left out (bridge spec 8.9). */
        if (clients[i].created != 0) {
            format_time(clients[i].created, when);
            cJSON_AddStringToObject(entry, "created", when);
        }
        if (clients[i].last_seen != 0) {
            format_time(clients[i].last_seen, when);
            cJSON_AddStringToObject(entry, "last_seen", when);
        }
        cJSON_AddBoolToObject(entry, "online", client_online(hub, clients[i].cid));
        cJSON_AddItemToArray(list, entry);
    }
    hub_reply_ok(hub, slot, id, body);
}

void hub_handle_auth_revoke(hub_t *hub, int slot, int id, const cJSON *message)
{
    if (!require_admin(hub, slot, id)) {
        return;
    }
    int cid = 0;
    if (!hub_json_int(message, "cid", &cid) || cid <= 0 || cid > 0xFFFF ||
        !hub_auth_revoke(&hub->auth, (uint16_t)cid)) {
        hub_reply_error(hub, slot, id, "bad_request", "no such client");
        return;
    }
    hub_reply_ok(hub, slot, id, NULL);
    /* The revoked client's live sessions close at once (Link spec 6.2). */
    for (size_t other = 0; other < hub->config.max_sessions; other++) {
        if (hub->sessions[other].state == HUB_SESSION_OPEN && hub->sessions[other].cid == cid) {
            hub_close_session(hub, (int)other, CLOSE_TOKEN_REVOKED);
        }
    }
}

void hub_handle_auth_set_role(hub_t *hub, int slot, int id, const cJSON *message)
{
    (void)message;
    /* With a fixed PIN every paired client is admin (bridge spec 8.9). */
    hub_reply_error(hub, slot, id, "unsupported", "every client of this hub is admin");
}

void hub_handle_hub_auth_set(hub_t *hub, int slot, int id, const cJSON *message)
{
    if (!require_admin(hub, slot, id)) {
        return;
    }
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(message, "mode");
    const cJSON *pin = cJSON_GetObjectItemCaseSensitive(message, "pin");
    hub_session_t *session = &hub->sessions[slot];

    if (cJSON_IsString(mode) && strcmp(mode->valuestring, "none") == 0) {
        /* Everyone stays connected and is admin (Link spec 7.12). */
        hub_auth_set_pin(&hub->auth, NULL);
        session->cid = 0;
        hub->io.advertise(hub->io.context);
        hub_reply_ok(hub, slot, id, NULL);
        return;
    }
    if (!cJSON_IsString(mode) || strcmp(mode->valuestring, "pin") != 0) {
        hub_reply_error(hub, slot, id, "bad_request", "mode must be none or pin");
        return;
    }
    if (pin == NULL) {
        hub_reply_error(hub, slot, id, "unsupported", "this hub uses a fixed PIN only");
        return;
    }
    if (!cJSON_IsString(pin) || !hub_auth_set_pin(&hub->auth, pin->valuestring)) {
        hub_reply_error(hub, slot, id, "bad_request", "the PIN must be six digits");
        return;
    }

    /* Every token is revoked. The client that set the PIN stays paired
     * with a fresh token; everyone else pairs again (Link spec 7.12). */
    char token[HUB_AUTH_TOKEN_CHARS + 1];
    hub_auth_issue(&hub->auth, session->client_name, token, &session->cid);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "token", token);
    hub_reply_ok(hub, slot, id, body);
    hub_close_sessions(hub, CLOSE_TOKEN_REVOKED, slot);
    hub->io.advertise(hub->io.context);
}

/* ---------------------------------------------------------------------------
 * Restart and reset (Link spec 7.13)
 * ------------------------------------------------------------------------- */

static void restart(hub_t *hub, int slot, int id, hub_restart_level_t level)
{
    hub_reply_ok(hub, slot, id, NULL);
    hub_close_sessions(hub, CLOSE_SHUTTING_DOWN, -1);
    hub->io.restart(hub->io.context, level);
}

void hub_handle_restart(hub_t *hub, int slot, int id, const cJSON *message)
{
    (void)message;
    if (require_admin(hub, slot, id)) {
        restart(hub, slot, id, HUB_RESTART_ONLY);
    }
}

void hub_handle_reset(hub_t *hub, int slot, int id, const cJSON *message)
{
    if (!require_admin(hub, slot, id)) {
        return;
    }
    const cJSON *level = cJSON_GetObjectItemCaseSensitive(message, "level");
    if (cJSON_IsString(level) && strcmp(level->valuestring, "network") == 0) {
        restart(hub, slot, id, HUB_RESTART_NETWORK);
    } else if (cJSON_IsString(level) && strcmp(level->valuestring, "factory") == 0) {
        restart(hub, slot, id, HUB_RESTART_FACTORY);
    } else {
        hub_reply_error(hub, slot, id, "bad_request", "level must be network or factory");
    }
}
