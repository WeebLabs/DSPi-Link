/*
 * setup_api.c - the setup network's pages and API. See setup_api.h;
 * implements bridge spec 9.2 and 9.3.
 *
 * Phase 4 covers Wi-Fi and the bridge's name. The PIN fields of bridge spec
 * 9.3 arrive with the PIN itself in phase 5; until then `pin_required` is
 * always false.
 */

#include "setup_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "cJSON.h"
#include "dspi_uart.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "hub_task.h"
#include "net.h"
#include "settings_model.h"
#include "web_assets.h"

static const char *TAG = "setup_api";

#define API_PREFIX "/setup/v1/"
#define SETUP_PAGE "/setup.html"
#define MAX_BODY_BYTES 512
#define MAX_NETWORKS 24

/* ---------------------------------------------------------------------------
 * Replies
 * ------------------------------------------------------------------------- */

static esp_err_t send_json(httpd_req_t *request, const char *status, cJSON *body)
{
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (text == NULL) {
        return httpd_resp_send_500(request);
    }
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(request, text);
    cJSON_free(text);
    return err;
}

/* {"error": code, "msg": plain sentence} with a 4xx status (bridge spec 9.3). */
static esp_err_t send_error(httpd_req_t *request, const char *status, const char *code,
                            const char *msg)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "error", code);
    cJSON_AddStringToObject(body, "msg", msg);
    return send_json(request, status, body);
}

/* Sends the browser to the setup page. This is the answer a phone's
 * connectivity check gets, so the phone shows the page (bridge spec 9.2). */
static esp_err_t redirect_to_setup(httpd_req_t *request)
{
    char location[40];
    snprintf(location, sizeof(location), "http://%s/", net_setup_address());
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", location);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, NULL, 0);
}

static esp_err_t send_asset(httpd_req_t *request, const web_asset_t *asset)
{
    httpd_resp_set_type(request, asset->content_type);
    httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(request, "Cache-Control", asset->immutable ? "max-age=86400" : "no-store");
    return httpd_resp_send(request, (const char *)asset->data, (ssize_t)asset->length);
}

/* ---------------------------------------------------------------------------
 * The API (bridge spec 9.3)
 * ------------------------------------------------------------------------- */

static esp_err_t get_state(httpd_req_t *request)
{
    char name[64];
    hub_task_name(name, sizeof(name));
    net_status_t network;
    net_get_status(&network);
    dspi_uart_status_t link;
    dspi_uart_get_status(&link);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "name", name);
    cJSON_AddStringToObject(body, "chip", board_table()->chip_name);
    cJSON_AddStringToObject(body, "version", esp_app_get_description()->version);
    cJSON_AddBoolToObject(body, "pin_required", false);

    cJSON *dspi = cJSON_AddObjectToObject(body, "dspi");
    cJSON_AddBoolToObject(dspi, "found", link.device_present);
    if (link.device_present) {
        cJSON_AddStringToObject(dspi, "serial", link.identity.serial);
        cJSON_AddNumberToObject(dspi, "baud", link.baud);
        cJSON_AddBoolToObject(dspi, "notify_enable", link.identity.notify_enabled);
    }

    cJSON *wifi = cJSON_AddObjectToObject(body, "wifi");
    if (network.saved_ssid[0] != '\0') {
        cJSON_AddStringToObject(wifi, "saved_ssid", network.saved_ssid);
    } else {
        cJSON_AddNullToObject(wifi, "saved_ssid");
    }
    cJSON_AddStringToObject(wifi, "join", net_join_name(network.join));
    if (network.join == NET_JOIN_FAILED) {
        cJSON_AddStringToObject(wifi, "failure", net_join_failure_name(network.join_failure));
    }
    if (network.connected) {
        cJSON_AddStringToObject(wifi, "ip", network.ip);
    } else {
        cJSON_AddNullToObject(wifi, "ip");
    }
    cJSON_AddStringToObject(wifi, "hostname", network.hostname);
    cJSON_AddStringToObject(wifi, "setup_ssid", network.setup_ssid);
    return send_json(request, "200 OK", body);
}

static esp_err_t get_networks(httpd_req_t *request)
{
    net_network_t *networks = calloc(MAX_NETWORKS, sizeof(net_network_t));
    if (networks == NULL) {
        return httpd_resp_send_500(request);
    }
    const size_t count = net_scan(networks, MAX_NETWORKS);

    cJSON *body = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(body, "networks");
    for (size_t i = 0; i < count; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "ssid", networks[i].ssid);
        cJSON_AddNumberToObject(entry, "rssi", networks[i].rssi);
        cJSON_AddBoolToObject(entry, "secure", networks[i].secure);
        cJSON_AddItemToArray(list, entry);
    }
    free(networks);
    return send_json(request, "200 OK", body);
}

/* Reads a small JSON body. NULL, with a reply already sent, on failure. */
static cJSON *read_json_body(httpd_req_t *request)
{
    if (request->content_len == 0 || request->content_len > MAX_BODY_BYTES) {
        send_error(request, "400 Bad Request", "bad_request", "The request needs a JSON body.");
        return NULL;
    }
    char body[MAX_BODY_BYTES + 1];
    size_t received = 0;
    while (received < request->content_len) {
        const int got = httpd_req_recv(request, body + received, request->content_len - received);
        if (got <= 0) {
            send_error(request, "400 Bad Request", "bad_request", "The body was cut short.");
            return NULL;
        }
        received += (size_t)got;
    }
    body[received] = '\0';
    cJSON *json = cJSON_Parse(body);
    if (!cJSON_IsObject(json)) {
        cJSON_Delete(json);
        send_error(request, "400 Bad Request", "bad_request", "The body is not a JSON object.");
        return NULL;
    }
    return json;
}

static const char *string_member(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

/* {"ssid", "password", "name"}: saves the name and starts joining. The
 * network is saved once the join works (bridge spec 9.3). */
static esp_err_t post_apply(httpd_req_t *request)
{
    cJSON *json = read_json_body(request);
    if (json == NULL) {
        return ESP_OK;
    }
    const char *ssid = string_member(json, "ssid");
    const char *password = string_member(json, "password");
    const char *name = string_member(json, "name");

    /* Check everything before changing anything. */
    esp_err_t result = ESP_OK;
    if (ssid == NULL || password == NULL) {
        result = send_error(request, "400 Bad Request", "bad_request",
                            "Choose a network and enter its password.");
    } else if (name != NULL && !settings_name_is_valid(name)) {
        result = send_error(request, "400 Bad Request", "bad_request",
                            "The name must be 1 to 63 characters.");
    } else if (net_join(ssid, password) != ESP_OK) {
        result = send_error(request, "400 Bad Request", "bad_request",
                            "Wi-Fi passwords are 8 to 63 characters, or empty for an open "
                            "network.");
    } else {
        if (name != NULL) {
            hub_task_rename(name);
        }
        ESP_LOGI(TAG, "Setup page: joining \"%s\"", ssid);
        result = send_json(request, "202 Accepted", cJSON_CreateObject());
    }
    cJSON_Delete(json);
    return result;
}

static esp_err_t post_finish(httpd_req_t *request)
{
    net_finish_setup();
    return send_json(request, "200 OK", cJSON_CreateObject());
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

/* True when the browser asked for the bridge by its setup address. Any
 * other host is a phone's connectivity check or a page the phone wanted
 * from the internet. */
static bool addressed_to_bridge(httpd_req_t *request)
{
    char host[64] = "";
    httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host));
    host[strcspn(host, ":")] = '\0';
    return strcmp(host, net_setup_address()) == 0;
}

esp_err_t setup_api_handle(httpd_req_t *request)
{
    char path[96];
    if (!request_path(request, path, sizeof(path))) {
        return redirect_to_setup(request);
    }
    /* Setup is rare and short, so every request is worth a log line when
     * a phone does not show the page. */
    char host[64] = "";
    httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host));
    ESP_LOGI(TAG, "%s http://%s%s", request->method == HTTP_GET ? "GET" : "POST", host, path);

    if (strncmp(path, API_PREFIX, strlen(API_PREFIX)) == 0) {
        const char *endpoint = path + strlen(API_PREFIX);
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

    if (request->method != HTTP_GET || !addressed_to_bridge(request)) {
        return redirect_to_setup(request);
    }
    const web_asset_t *asset = web_assets_find(strcmp(path, "/") == 0 ? SETUP_PAGE : path);
    return asset != NULL ? send_asset(request, asset) : redirect_to_setup(request);
}
