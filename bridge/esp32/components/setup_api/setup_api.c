/*
 * setup_api.c - the setup network's pages and API. See setup_api.h;
 * implements bridge spec 9.2 and 9.3.
 *
 * While the PIN is on, which is only possible when the setup network has
 * come back in FALLBACK, every request that changes something must carry
 * it as `current_pin` (bridge spec 9.2).
 */

#include "setup_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "board.h"
#include "bridge_config.h"
#include "cJSON.h"
#include "dspi_uart.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "http_server.h"
#include "hub_task.h"
#include "net.h"
#include "settings_model.h"
#include "web_assets.h"

static const char *TAG = "setup_api";

#define API_PREFIX "/setup/v1/"
#define SETUP_PAGE "/setup.html"

/* ---------------------------------------------------------------------------
 * Replies
 * ------------------------------------------------------------------------- */

/* Sets a reply's status, type and Cache-Control, logging a failure. */
static esp_err_t prepare_reply(httpd_req_t *request, const char *status, const char *type,
                               const char *cache_control)
{
    esp_err_t err = httpd_resp_set_status(request, status);
    if (err == ESP_OK && type != NULL) {
        err = httpd_resp_set_type(request, type);
    }
    if (err == ESP_OK) {
        err = httpd_resp_set_hdr(request, "Cache-Control", cache_control);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not prepare a reply: %s", esp_err_to_name(err));
    }
    return err;
}

/* Sends `body` and deletes it; NULL (memory ran out building it) is a 500. */
static esp_err_t send_json(httpd_req_t *request, const char *status, cJSON *body)
{
    char *text = body != NULL ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    if (text == NULL) {
        ESP_LOGW(TAG, "No memory for a reply");
        return httpd_resp_send_500(request);
    }
    esp_err_t err = prepare_reply(request, status, "application/json", "no-store");
    if (err == ESP_OK) {
        err = httpd_resp_sendstr(request, text);
    }
    cJSON_free(text);
    return err;
}

/* {"error": code, "msg": plain sentence} (bridge spec 9.3). */
static esp_err_t send_error(httpd_req_t *request, const char *status, const char *code,
                            const char *msg)
{
    cJSON *body = cJSON_CreateObject();
    if (cJSON_AddStringToObject(body, "error", code) == NULL ||
        cJSON_AddStringToObject(body, "msg", msg) == NULL) {
        cJSON_Delete(body);
        body = NULL;
    }
    return send_json(request, status, body);
}

static esp_err_t send_bad_request(httpd_req_t *request, const char *msg)
{
    return send_error(request, "400 Bad Request", "bad_request", msg);
}

/* Sends the browser to the setup page. This is the answer a phone's
 * connectivity check gets, so the phone shows the page (bridge spec 9.2). */
static esp_err_t redirect_to_setup(httpd_req_t *request)
{
    char location[40];
    snprintf(location, sizeof(location), "http://%s/", net_setup_address());
    esp_err_t err = prepare_reply(request, "302 Found", NULL, "no-store");
    if (err == ESP_OK) {
        err = httpd_resp_set_hdr(request, "Location", location);
    }
    return err == ESP_OK ? httpd_resp_send(request, NULL, 0) : err;
}

static esp_err_t send_asset(httpd_req_t *request, const web_asset_t *asset)
{
    esp_err_t err = prepare_reply(request, "200 OK", asset->content_type,
                                  asset->immutable ? "max-age=86400" : "no-store");
    if (err == ESP_OK) {
        err = httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
    }
    return err == ESP_OK
               ? httpd_resp_send(request, (const char *)asset->data, (ssize_t)asset->length)
               : err;
}

/* ---------------------------------------------------------------------------
 * The API (bridge spec 9.3)
 * ------------------------------------------------------------------------- */

/* Adds `value` as a string, or null when `present` is false. False if
 * memory ran out. */
static bool add_string_or_null(cJSON *object, const char *name, const char *value, bool present)
{
    return (present ? cJSON_AddStringToObject(object, name, value)
                    : cJSON_AddNullToObject(object, name)) != NULL;
}

static bool add_dspi(cJSON *body)
{
    dspi_uart_status_t link;
    dspi_uart_get_status(&link);
    cJSON *dspi = cJSON_AddObjectToObject(body, "dspi");
    bool ok = cJSON_AddBoolToObject(dspi, "found", link.device_present) != NULL;
    if (link.device_present) {
        ok = cJSON_AddStringToObject(dspi, "serial", link.identity.serial) != NULL && ok;
        ok = cJSON_AddNumberToObject(dspi, "baud", link.baud) != NULL && ok;
        ok = cJSON_AddBoolToObject(dspi, "notify_enable", link.identity.notify_enabled) != NULL &&
             ok;
    }
    return ok;
}

static bool add_wifi(cJSON *body)
{
    net_status_t network;
    net_get_status(&network);
    cJSON *wifi = cJSON_AddObjectToObject(body, "wifi");
    bool ok =
        add_string_or_null(wifi, "saved_ssid", network.saved_ssid, network.saved_ssid[0] != '\0');
    ok = cJSON_AddStringToObject(wifi, "join", net_join_name(network.join)) != NULL && ok;
    if (network.join == NET_JOIN_FAILED) {
        ok = cJSON_AddStringToObject(wifi, "failure",
                                     net_join_failure_name(network.join_failure)) != NULL &&
             ok;
    }
    ok = add_string_or_null(wifi, "ip", network.ip, network.connected) && ok;
    ok = cJSON_AddStringToObject(wifi, "hostname", network.hostname) != NULL && ok;
    ok = cJSON_AddStringToObject(wifi, "setup_ssid", network.setup_ssid) != NULL && ok;
    return ok;
}

static esp_err_t get_state(httpd_req_t *request)
{
    char name[SETTINGS_NAME_MAX_BYTES + 1];
    hub_task_name(name, sizeof(name));

    cJSON *body = cJSON_CreateObject();
    bool ok = cJSON_AddStringToObject(body, "name", name) != NULL;
    ok = cJSON_AddStringToObject(body, "chip", board_table()->chip_name) != NULL && ok;
    ok = cJSON_AddStringToObject(body, "version", esp_app_get_description()->version) != NULL && ok;
    ok = cJSON_AddBoolToObject(body, "pin_required", hub_task_pin_enabled()) != NULL && ok;
    ok = add_dspi(body) && ok;
    ok = add_wifi(body) && ok;
    if (!ok) {
        cJSON_Delete(body);
        body = NULL;
    }
    return send_json(request, "200 OK", body);
}

/* {"networks": [{"ssid", "rssi", "secure"}, ...]}, or NULL if memory ran
 * out. */
static cJSON *networks_document(const net_network_t *networks, size_t count)
{
    cJSON *body = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(body, "networks");
    bool ok = list != NULL;
    for (size_t i = 0; i < count && ok; i++) {
        cJSON *entry = cJSON_CreateObject();
        ok = cJSON_AddItemToArray(list, entry);
        ok = ok && cJSON_AddStringToObject(entry, "ssid", networks[i].ssid) != NULL;
        ok = ok && cJSON_AddNumberToObject(entry, "rssi", networks[i].rssi) != NULL;
        ok = ok && cJSON_AddBoolToObject(entry, "secure", networks[i].secure) != NULL;
    }
    if (!ok) {
        cJSON_Delete(body);
        return NULL;
    }
    return body;
}

static esp_err_t get_networks(httpd_req_t *request)
{
    net_network_t *networks = calloc(SETUP_API_MAX_NETWORKS, sizeof(net_network_t));
    if (networks == NULL) {
        ESP_LOGW(TAG, "No memory to list networks");
        return httpd_resp_send_500(request);
    }
    size_t count = 0;
    const esp_err_t err = net_scan_networks(networks, SETUP_API_MAX_NETWORKS, &count);
    cJSON *body = err == ESP_OK ? networks_document(networks, count) : NULL;
    free(networks);
    if (err != ESP_OK) {
        /* Not an empty list: the page would say no networks are in range. */
        return send_error(request, "503 Service Unavailable", "busy",
                          "The bridge could not look for networks just then. Try again.");
    }
    return send_json(request, "200 OK", body);
}

/* Reads a small JSON body. NULL, with a reply already sent, on failure. */
static cJSON *read_json_body(httpd_req_t *request)
{
    if (request->content_len == 0 || request->content_len > SETUP_API_MAX_BODY_BYTES) {
        send_bad_request(request, "The request needs a JSON body.");
        return NULL;
    }
    char body[SETUP_API_MAX_BODY_BYTES + 1];
    size_t received = 0;
    while (received < request->content_len) {
        const int got = httpd_req_recv(request, body + received, request->content_len - received);
        if (got <= 0) {
            send_bad_request(request, "The body was cut short.");
            return NULL;
        }
        received += (size_t)got;
    }
    body[received] = '\0';
    cJSON *json = cJSON_Parse(body);
    if (!cJSON_IsObject(json)) {
        cJSON_Delete(json);
        send_bad_request(request, "The body is not a JSON object.");
        return NULL;
    }
    return json;
}

static const char *string_member(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

/* While the PIN is on, a changing request must carry it. Returns true when
 * the request may go on; otherwise the reply has been sent. */
static bool pin_allows(httpd_req_t *request, const cJSON *json, esp_err_t *result)
{
    if (!hub_task_pin_enabled()) {
        return true;
    }
    const char *current = json != NULL ? string_member(json, "current_pin") : NULL;
    if (current == NULL) {
        *result = send_error(request, "403 Forbidden", "pin_required", "Enter the bridge's PIN.");
        return false;
    }
    const uint32_t address = http_server_peer_address(httpd_req_to_sockfd(request));
    switch (hub_task_check_pin(address, current)) {
    case HUB_AUTH_OK:
        return true;
    case HUB_AUTH_RATE_LIMITED:
        *result = send_error(request, "429 Too Many Requests", "rate_limited",
                             "Too many wrong PINs. Wait a minute and try again.");
        return false;
    case HUB_AUTH_UNAVAILABLE:
        /* The PIN could not be checked at all; refuse without blaming it. */
        *result = send_error(request, "503 Service Unavailable", "busy",
                             "The bridge is busy. Try again in a moment.");
        return false;
    default:
        *result = send_error(request, "403 Forbidden", "wrong_pin", "That PIN is not right.");
        return false;
    }
}

/* The choices an apply request makes (bridge spec 9.3). */
typedef struct {
    const char *ssid;
    const char *password;
    const char *name;  /* NULL keeps the name */
    bool turn_pin_on;  /* With `pin` */
    bool turn_pin_off; /* The PIN is on and the page turned it off */
    const char *pin;
} apply_choices_t;

/* What is wrong with the choices, as a sentence for the page, or NULL. */
static const char *apply_problem(const apply_choices_t *choices)
{
    if (choices->turn_pin_on && !hub_auth_pin_is_valid(choices->pin)) {
        return "The PIN must be six digits.";
    }
    if (choices->ssid == NULL || choices->password == NULL) {
        return "Choose a network and enter its password.";
    }
    if (choices->name != NULL && !settings_name_is_valid(choices->name)) {
        return "The name must be 1 to 63 bytes of text.";
    }
    switch (net_check_credentials(choices->ssid, choices->password)) {
    case NET_CREDENTIALS_BAD_SSID:
        return "The network name must be 1 to 32 bytes.";
    case NET_CREDENTIALS_BAD_PASSWORD:
        return "Wi-Fi passwords are 8 to 63 characters, or 64 hexadecimal digits, or empty "
               "for an open network.";
    case NET_CREDENTIALS_OK:
        break;
    }
    return NULL;
}

/* Saves the name and the PIN choice, then starts joining. Everything has
 * been checked, so a failure here is the bridge's, not the page's. */
static esp_err_t carry_out_apply(httpd_req_t *request, const apply_choices_t *choices)
{
    if (choices->name != NULL && hub_task_rename(choices->name) != ESP_OK) {
        return send_error(request, "503 Service Unavailable", "busy",
                          "The bridge could not save its name. Try again.");
    }
    if ((choices->turn_pin_on || choices->turn_pin_off) &&
        hub_task_set_pin(choices->turn_pin_on ? choices->pin : NULL) != ESP_OK) {
        return send_error(request, "503 Service Unavailable", "busy",
                          "The bridge could not change its PIN. Try again.");
    }
    if (net_join(choices->ssid, choices->password) != ESP_OK) {
        return send_error(request, "503 Service Unavailable", "busy",
                          "The bridge could not start joining the network. Try again.");
    }
    ESP_LOGI(TAG, "Setup page: joining \"%s\"", choices->ssid);
    return send_json(request, "202 Accepted", cJSON_CreateObject());
}

/* {"ssid", "password", "name", "pin_enabled", "pin", "current_pin"}: saves
 * the name and the PIN choice and starts joining. The network is saved once
 * the join works (bridge spec 9.3). */
static esp_err_t post_apply(httpd_req_t *request)
{
    cJSON *json = read_json_body(request);
    if (json == NULL) {
        return ESP_OK;
    }
    esp_err_t result = ESP_OK;
    if (!pin_allows(request, json, &result)) {
        cJSON_Delete(json);
        return result;
    }
    /* A missing pin_enabled leaves the PIN as it is. */
    const cJSON *pin_enabled = cJSON_GetObjectItemCaseSensitive(json, "pin_enabled");
    const apply_choices_t choices = {
        .ssid = string_member(json, "ssid"),
        .password = string_member(json, "password"),
        .name = string_member(json, "name"),
        .turn_pin_on = cJSON_IsTrue(pin_enabled),
        .turn_pin_off = cJSON_IsFalse(pin_enabled) && hub_task_pin_enabled(),
        .pin = string_member(json, "pin"),
    };

    /* Check everything before changing anything. */
    const char *problem = apply_problem(&choices);
    result =
        problem != NULL ? send_bad_request(request, problem) : carry_out_apply(request, &choices);
    cJSON_Delete(json);
    return result;
}

static esp_err_t post_finish(httpd_req_t *request)
{
    esp_err_t result = ESP_OK;
    cJSON *json = request->content_len > 0 ? read_json_body(request) : NULL;
    if (request->content_len > 0 && json == NULL) {
        return ESP_OK; /* read_json_body has replied */
    }
    if (pin_allows(request, json, &result)) {
        result = net_finish_setup() == ESP_OK
                     ? send_json(request, "200 OK", cJSON_CreateObject())
                     : send_error(request, "503 Service Unavailable", "busy",
                                  "The bridge could not finish setting up. Try again.");
    }
    cJSON_Delete(json);
    return result;
}

/* ---------------------------------------------------------------------------
 * Routing
 * ------------------------------------------------------------------------- */

/* The path without its query string, in `path`. False if it is too long. */
static bool request_path(const httpd_req_t *request, char *path, size_t size)
{
    const size_t length = strcspn(request->uri, "?");
    if (length >= size) {
        return false;
    }
    memcpy(path, request->uri, length);
    path[length] = '\0';
    return true;
}

/* The Host header without its port, or "" when there is none or it is too
 * long to be the setup address. */
static void request_host(httpd_req_t *request, char *host, size_t size)
{
    if (httpd_req_get_hdr_value_str(request, "Host", host, size) != ESP_OK) {
        host[0] = '\0';
    }
    host[strcspn(host, ":")] = '\0';
}

/* True when the request's Content-Type is application/json, with or
 * without parameters such as a charset. */
static bool is_json_request(httpd_req_t *request)
{
    static const char JSON_TYPE[] = "application/json";
    char type[64];
    if (httpd_req_get_hdr_value_str(request, "Content-Type", type, sizeof(type)) != ESP_OK) {
        return false; /* Missing, or too long to be this */
    }
    const size_t length = sizeof(JSON_TYPE) - 1;
    return strncasecmp(type, JSON_TYPE, length) == 0 &&
           (type[length] == '\0' || type[length] == ';' || type[length] == ' ');
}

/* The API answers only the setup page itself (bridge spec 9.3). The captive
 * DNS gives every name the bridge's address, so a page from anywhere,
 * still open in a browser on the setup network, could otherwise call the
 * API under its own name (DNS rebinding) and read the answers. A browser
 * sends a cross-site POST without asking the server first only for three
 * form content types; application/json is not one, and port 80 answers no
 * such question, so requiring it stops a page elsewhere posting blind. */
static esp_err_t route_api(httpd_req_t *request, const char *host, const char *endpoint)
{
    if (strcmp(host, net_setup_address()) != 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Open the setup page at http://%s/.", net_setup_address());
        return send_bad_request(request, msg);
    }
    if (request->method != HTTP_GET && !is_json_request(request)) {
        return send_bad_request(request, "The request must be sent as application/json.");
    }
    const bool is_get = request->method == HTTP_GET;
    if (is_get && strcmp(endpoint, "state") == 0) {
        return get_state(request);
    }
    if (is_get && strcmp(endpoint, "networks") == 0) {
        return get_networks(request);
    }
    if (!is_get && strcmp(endpoint, "apply") == 0) {
        return post_apply(request);
    }
    if (!is_get && strcmp(endpoint, "finish") == 0) {
        return post_finish(request);
    }
    return send_error(request, "404 Not Found", "not_found", "No such setup request.");
}

esp_err_t setup_api_handle(httpd_req_t *request)
{
    char path[96];
    if (!request_path(request, path, sizeof(path))) {
        return redirect_to_setup(request);
    }
    /* Setup is rare and short, so every request is worth a log line when
     * a phone does not show the page. */
    char host[64];
    request_host(request, host, sizeof(host));
    ESP_LOGI(TAG, "%s http://%s%s", request->method == HTTP_GET ? "GET" : "POST", host, path);

    if (strncmp(path, API_PREFIX, strlen(API_PREFIX)) == 0) {
        return route_api(request, host, path + strlen(API_PREFIX));
    }
    /* Any other host is a phone's connectivity check or a page the phone
     * wanted from the internet. */
    if (request->method != HTTP_GET || strcmp(host, net_setup_address()) != 0) {
        return redirect_to_setup(request);
    }
    const web_asset_t *asset = web_assets_find(strcmp(path, "/") == 0 ? SETUP_PAGE : path);
    return asset != NULL ? send_asset(request, asset) : redirect_to_setup(request);
}
