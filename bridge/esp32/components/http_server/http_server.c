/*
 * http_server.c - the Link port and port 80. See http_server.h; implements
 * bridge spec 8.2 and 8.8 and Link spec 3.2, 4 and 4.2.
 *
 * The slot table is the only state shared between tasks here: the HTTP
 * server's task opens and closes slots, while the hub task sends to them.
 * Every access takes `slots_lock` for a few instructions.
 */

#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bridge_config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "web_assets.h"

static const char *TAG = "http_server";

#define LINK_SUBPROTOCOL "dspi-link-1"
#define CLOSE_PROTOCOL_ERROR 4000
#define CLOSE_CLIENT_STOPPED_READING 1011

/* Sockets the Link server keeps beside its sessions, for page loads and the
 * info endpoint. */
#define EXTRA_HTTP_SOCKETS 3
#define SERVER_STACK_BYTES 6144

/* ---------------------------------------------------------------------------
 * Slots
 * ------------------------------------------------------------------------- */

typedef enum {
    SLOT_FREE,
    SLOT_OPEN,
    SLOT_CLOSING, /* Socket closed; waiting for http_server_release_slot() */
} slot_state_t;

typedef struct {
    slot_state_t state;
    int fd;
    uint32_t queued;          /* Messages handed to the server, not yet sent */
    int64_t last_progress_us; /* When the queue last moved, while not empty */
} slot_t;

static struct {
    http_server_config_t config;
    httpd_handle_t link_server;
    httpd_handle_t redirect_server;
    slot_t slots[LINK_MAX_SESSIONS_WITH_PSRAM];
    portMUX_TYPE slots_lock;
} server_state = {.slots_lock = portMUX_INITIALIZER_UNLOCKED};

/* The open slot for `fd`, or -1. Call with slots_lock held. */
static int find_slot_locked(int fd)
{
    for (size_t i = 0; i < server_state.config.max_sessions; i++) {
        if (server_state.slots[i].state == SLOT_OPEN && server_state.slots[i].fd == fd) {
            return (int)i;
        }
    }
    return -1;
}

static int find_slot(int fd)
{
    taskENTER_CRITICAL(&server_state.slots_lock);
    const int slot = find_slot_locked(fd);
    taskEXIT_CRITICAL(&server_state.slots_lock);
    return slot;
}

static bool has_free_slot(void)
{
    bool found = false;
    taskENTER_CRITICAL(&server_state.slots_lock);
    for (size_t i = 0; i < server_state.config.max_sessions && !found; i++) {
        found = server_state.slots[i].state == SLOT_FREE;
    }
    taskEXIT_CRITICAL(&server_state.slots_lock);
    return found;
}

static int open_slot(int fd)
{
    int slot = -1;
    taskENTER_CRITICAL(&server_state.slots_lock);
    for (size_t i = 0; i < server_state.config.max_sessions && slot < 0; i++) {
        if (server_state.slots[i].state == SLOT_FREE) {
            server_state.slots[i] = (slot_t){.state = SLOT_OPEN, .fd = fd};
            slot = (int)i;
        }
    }
    taskEXIT_CRITICAL(&server_state.slots_lock);
    return slot;
}

/* The fd of an open slot, or -1. */
static int slot_fd(int slot)
{
    if (slot < 0 || (size_t)slot >= server_state.config.max_sessions) {
        return -1;
    }
    taskENTER_CRITICAL(&server_state.slots_lock);
    const slot_t *entry = &server_state.slots[slot];
    const int fd = entry->state == SLOT_OPEN ? entry->fd : -1;
    taskEXIT_CRITICAL(&server_state.slots_lock);
    return fd;
}

void http_server_release_slot(int slot)
{
    if (slot < 0 || (size_t)slot >= server_state.config.max_sessions) {
        return;
    }
    taskENTER_CRITICAL(&server_state.slots_lock);
    if (server_state.slots[slot].state == SLOT_CLOSING) {
        server_state.slots[slot].state = SLOT_FREE;
    }
    taskEXIT_CRITICAL(&server_state.slots_lock);
}

/* Called by the server whenever any socket closes, WebSocket or not. */
static void on_socket_closed(httpd_handle_t server, int fd)
{
    (void)server;
    int slot = -1;
    taskENTER_CRITICAL(&server_state.slots_lock);
    slot = find_slot_locked(fd);
    if (slot >= 0) {
        server_state.slots[slot].state = SLOT_CLOSING;
    }
    taskEXIT_CRITICAL(&server_state.slots_lock);

    if (slot >= 0) {
        server_state.config.handlers.session_closed(server_state.config.handlers.context, slot);
    }
    close(fd);
}

/* ---------------------------------------------------------------------------
 * Sending
 * ------------------------------------------------------------------------- */

/* One message on its way out; freed when the server reports it sent. */
typedef struct {
    int slot;
    int fd;
    size_t length;
    uint8_t data[];
} outgoing_t;

static void on_sent(esp_err_t err, int fd, void *argument)
{
    (void)err; /* A failed send closes the socket, which on_socket_closed handles */
    outgoing_t *message = argument;
    taskENTER_CRITICAL(&server_state.slots_lock);
    slot_t *slot = &server_state.slots[message->slot];
    if (slot->fd == fd && slot->queued > 0) {
        slot->queued--;
        slot->last_progress_us = esp_timer_get_time();
    }
    taskEXIT_CRITICAL(&server_state.slots_lock);
    free(message);
}

/* Queues one frame of any type for a slot, within its queue limit. */
static bool queue_frame(int slot, httpd_ws_type_t type, const uint8_t *data, size_t length)
{
    const int fd = slot_fd(slot);
    if (fd < 0) {
        return false;
    }

    taskENTER_CRITICAL(&server_state.slots_lock);
    slot_t *entry = &server_state.slots[slot];
    const bool room = entry->queued < LINK_SESSION_QUEUE_FRAMES;
    if (room) {
        if (entry->queued == 0) {
            entry->last_progress_us = esp_timer_get_time();
        }
        entry->queued++;
    }
    taskEXIT_CRITICAL(&server_state.slots_lock);
    if (!room) {
        return false;
    }

    outgoing_t *message = malloc(sizeof(outgoing_t) + length);
    if (message != NULL) {
        message->slot = slot;
        message->fd = fd;
        message->length = length;
        memcpy(message->data, data, length);
        httpd_ws_frame_t frame = {
            .final = true, .type = type, .payload = message->data, .len = length};
        if (httpd_ws_send_data_async(server_state.link_server, fd, &frame, on_sent, message) ==
            ESP_OK) {
            return true;
        }
        free(message);
    }

    taskENTER_CRITICAL(&server_state.slots_lock);
    entry->queued--;
    taskEXIT_CRITICAL(&server_state.slots_lock);
    return false;
}

bool http_server_send(int slot, bool binary, const uint8_t *data, size_t length)
{
    return queue_frame(slot, binary ? HTTPD_WS_TYPE_BINARY : HTTPD_WS_TYPE_TEXT, data, length);
}

void http_server_ping(int slot)
{
    queue_frame(slot, HTTPD_WS_TYPE_PING, NULL, 0);
}

void http_server_close(int slot, uint16_t code)
{
    const int fd = slot_fd(slot);
    if (fd < 0) {
        return;
    }
    /* The close frame and the close itself are queued on the server's task
     * in this order, so the frame goes out first. */
    const uint8_t payload[2] = {(uint8_t)(code >> 8), (uint8_t)(code & 0xFF)};
    queue_frame(slot, HTTPD_WS_TYPE_CLOSE, payload, sizeof(payload));
    httpd_sess_trigger_close(server_state.link_server, fd);
}

void http_server_close_stalled(void)
{
    const int64_t now_us = esp_timer_get_time();
    for (size_t i = 0; i < server_state.config.max_sessions; i++) {
        taskENTER_CRITICAL(&server_state.slots_lock);
        const slot_t entry = server_state.slots[i];
        taskEXIT_CRITICAL(&server_state.slots_lock);

        const bool stalled =
            entry.state == SLOT_OPEN && entry.queued > 0 &&
            now_us - entry.last_progress_us >= (int64_t)LINK_UNWRITABLE_TIMEOUT_MS * 1000;
        if (stalled) {
            ESP_LOGW(TAG, "Session %u stopped reading; closing it", (unsigned)i);
            http_server_close((int)i, CLOSE_CLIENT_STOPPED_READING);
        }
    }
}

/* ---------------------------------------------------------------------------
 * The Link WebSocket (Link spec 4)
 * ------------------------------------------------------------------------- */

/* Before the handshake: a full bridge answers 503 (bridge spec 8.2). */
static esp_err_t before_handshake(httpd_req_t *request)
{
    if (has_free_slot()) {
        return ESP_OK;
    }
    httpd_resp_set_status(request, "503 Service Unavailable");
    httpd_resp_sendstr(request, "This bridge has no room for another session.");
    return ESP_FAIL;
}

/* After the handshake: the session begins. ESP-IDF calls this, not the
 * frame handler, once the 101 response has gone out. */
static esp_err_t after_handshake(httpd_req_t *request)
{
    const http_server_handlers_t *handlers = &server_state.config.handlers;
    const int slot = open_slot(httpd_req_to_sockfd(request));
    if (slot < 0) {
        return ESP_FAIL; /* The server closes the socket */
    }
    handlers->session_opened(handlers->context, slot);
    return ESP_OK;
}

/* Answers a client's control frame: a pong for a ping, a close for a close. */
static void answer_control_frame(httpd_req_t *request, int slot, const httpd_ws_frame_t *frame)
{
    const http_server_handlers_t *handlers = &server_state.config.handlers;
    if (frame->type == HTTPD_WS_TYPE_PING) {
        httpd_ws_frame_t pong = {.final = true,
                                 .type = HTTPD_WS_TYPE_PONG,
                                 .payload = frame->payload,
                                 .len = frame->len};
        httpd_ws_send_frame(request, &pong);
        handlers->session_activity(handlers->context, slot);
    } else if (frame->type == HTTPD_WS_TYPE_PONG) {
        handlers->session_activity(handlers->context, slot);
    } else if (frame->type == HTTPD_WS_TYPE_CLOSE) {
        httpd_ws_frame_t reply = {.final = true,
                                  .type = HTTPD_WS_TYPE_CLOSE,
                                  .payload = frame->payload,
                                  .len = frame->len >= 2 ? 2 : 0};
        httpd_ws_send_frame(request, &reply);
        httpd_sess_trigger_close(server_state.link_server, httpd_req_to_sockfd(request));
    }
}

/* Called for every frame a client sends. Each frame must be read in full
 * before returning, or the server loses its place in the stream; returning
 * an error instead makes the server close the socket. */
static esp_err_t on_link_socket(httpd_req_t *request)
{
    const http_server_handlers_t *handlers = &server_state.config.handlers;
    const int fd = httpd_req_to_sockfd(request);

    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(request, &frame, 0); /* Learn the length */
    if (err != ESP_OK) {
        return err;
    }
    const int slot = find_slot(fd);
    if (slot < 0) {
        return ESP_FAIL; /* No session for this socket: close it */
    }
    if (frame.len > LINK_MAX_FRAME_BYTES || frame.type == HTTPD_WS_TYPE_CONTINUE) {
        /* Too big, or fragmented, which no Link client sends (Link spec 4). */
        http_server_close(slot, CLOSE_PROTOCOL_ERROR);
        return ESP_FAIL;
    }

    uint8_t *data = malloc(frame.len + 1);
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    frame.payload = data;
    if (frame.len > 0) {
        err = httpd_ws_recv_frame(request, &frame, frame.len);
        if (err != ESP_OK) {
            free(data);
            return err;
        }
    }
    data[frame.len] = '\0';

    if (frame.type == HTTPD_WS_TYPE_TEXT || frame.type == HTTPD_WS_TYPE_BINARY) {
        /* Ownership of `data` passes to the handler. */
        handlers->session_message(handlers->context, slot, frame.type == HTTPD_WS_TYPE_BINARY, data,
                                  frame.len);
        return ESP_OK;
    }
    answer_control_frame(request, slot, &frame);
    free(data);
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * The info endpoint and web pages
 * ------------------------------------------------------------------------- */

static esp_err_t on_info(httpd_req_t *request)
{
    const http_server_handlers_t *handlers = &server_state.config.handlers;
    char *json = handlers->info_json(handlers->context);
    if (json == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    /* Only this endpoint allows other origins (Link spec 3.2). */
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(request, json);
    free(json);
    return err;
}

static esp_err_t on_web_page(httpd_req_t *request)
{
    /* The path without any query string; "/" serves the index page. */
    char path[96];
    const size_t path_length = strcspn(request->uri, "?");
    if (path_length >= sizeof(path)) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Not found");
    }
    memcpy(path, request->uri, path_length);
    path[path_length] = '\0';
    const web_asset_t *asset = web_assets_find(strcmp(path, "/") == 0 ? "/index.html" : path);
    if (asset == NULL) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Not found");
    }

    char if_none_match[48] = "";
    if (httpd_req_get_hdr_value_str(request, "If-None-Match", if_none_match,
                                    sizeof(if_none_match)) == ESP_OK &&
        strcmp(if_none_match, asset->etag) == 0) {
        httpd_resp_set_status(request, "304 Not Modified");
        return httpd_resp_send(request, NULL, 0);
    }

    httpd_resp_set_type(request, asset->content_type);
    httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(request, "ETag", asset->etag);
    /* Vite names assets by their hash, so those never change; pages may. */
    httpd_resp_set_hdr(request, "Cache-Control",
                       asset->immutable ? "public, max-age=31536000, immutable" : "no-cache");
    return httpd_resp_send(request, (const char *)asset->data, (ssize_t)asset->length);
}

/* ---------------------------------------------------------------------------
 * Port 80
 * ------------------------------------------------------------------------- */

/* Sends a browser that typed the plain hostname to the Link port (bridge
 * spec 8.8). */
static esp_err_t on_redirect(httpd_req_t *request)
{
    char host[72] = "";
    if (httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "No Host header");
    }
    char *port = strchr(host, ':');
    if (port != NULL) {
        *port = '\0';
    }
    char location[96];
    snprintf(location, sizeof(location), "http://%s:%d/", host, LINK_TCP_PORT);
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", location);
    return httpd_resp_send(request, NULL, 0);
}

/* ---------------------------------------------------------------------------
 * Start
 * ------------------------------------------------------------------------- */

static esp_err_t start_link_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = LINK_TCP_PORT;
    config.max_open_sockets = server_state.config.max_sessions + EXTRA_HTTP_SOCKETS;
    config.stack_size = SERVER_STACK_BYTES;
    config.close_fn = on_socket_closed;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&server_state.link_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    /* Registered most specific first: the wildcard catches the rest. */
    const httpd_uri_t link_socket = {
        .uri = "/dspi/v1",
        .method = HTTP_GET,
        .handler = on_link_socket,
        .is_websocket = true,
        .handle_ws_control_frames = true, /* Pongs prove a client alive (Link spec 4) */
        .supported_subprotocol = LINK_SUBPROTOCOL,
        .ws_pre_handshake_cb = before_handshake,
        .ws_post_handshake_cb = after_handshake,
    };
    const httpd_uri_t info = {.uri = "/dspi/v1/info", .method = HTTP_GET, .handler = on_info};
    const httpd_uri_t pages = {.uri = "/*", .method = HTTP_GET, .handler = on_web_page};

    err = httpd_register_uri_handler(server_state.link_server, &link_socket);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server_state.link_server, &info);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server_state.link_server, &pages);
    }
    return err;
}

static esp_err_t start_redirect_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_REDIRECT_PORT;
    config.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1; /* Each server needs its own */
    config.max_open_sockets = 2;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&server_state.redirect_server, &config);
    if (err != ESP_OK) {
        return err;
    }
    const httpd_uri_t redirect = {.uri = "/*", .method = HTTP_GET, .handler = on_redirect};
    return httpd_register_uri_handler(server_state.redirect_server, &redirect);
}

esp_err_t http_server_start(const http_server_config_t *config)
{
    server_state.config = *config;
    if (server_state.config.max_sessions > LINK_MAX_SESSIONS_WITH_PSRAM) {
        server_state.config.max_sessions = LINK_MAX_SESSIONS_WITH_PSRAM;
    }

    esp_err_t err = start_link_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the Link server on port %d: %s", LINK_TCP_PORT,
                 esp_err_to_name(err));
        return err;
    }
    err = start_redirect_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the redirect on port %d: %s", HTTP_REDIRECT_PORT,
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Serving Link on port %d and a redirect on port %d", LINK_TCP_PORT,
             HTTP_REDIRECT_PORT);
    return ESP_OK;
}
