/*
 * http_server.c - the Link port and port 80. See http_server.h; implements
 * bridge spec 8.2 and 8.8 and Link spec 3.2, 4 and 4.2.
 *
 * The slot table, with each session's queue of frames to send, is the only
 * state shared between tasks here: the HTTP server's task opens, closes and
 * sends, while the hub task queues frames. Every access takes `slots_lock`
 * for a few instructions; nothing is allocated or sent while it is held.
 */

#include "http_server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bridge_config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lwip/sockets.h"
#include "net.h"
#include "web_assets.h"
#include "ws_header.h"

static const char *TAG = "http_server";

#define LINK_SUBPROTOCOL "dspi-link-1"
#define CLOSE_PROTOCOL_ERROR 4000

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

/* One frame waiting to be sent. */
typedef struct {
    uint8_t opcode; /* WS_OPCODE_* */
    size_t length;
    uint8_t data[];
} outgoing_t;

typedef struct {
    slot_state_t state;
    int fd;
    bool close_queued;    /* A close frame is waiting; nothing more is accepted */
    bool close_requested; /* The socket has been asked to close; ask only once */

    /* Frames waiting to be sent, oldest first (bridge spec 8.2). */
    outgoing_t *queue[LINK_SESSION_QUEUE_FRAMES];
    size_t queue_head;
    size_t queue_count;
    int64_t last_progress_us; /* When bytes last went out, while any wait */

    /* The batch being sent: frames already encoded, of which `partial_sent`
     * bytes have gone. Created and freed only on the server's task. */
    uint8_t *partial;
    size_t partial_length;
    size_t partial_sent;
    bool partial_ends_with_close;
} slot_t;

static struct {
    http_server_config_t config;
    httpd_handle_t link_server;
    httpd_handle_t redirect_server;
    slot_t slots[LINK_MAX_SESSIONS_WITH_PSRAM];
    bool pump_scheduled; /* send_pump() is queued on the server's task */
    size_t pump_next_slot;
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

static bool valid_slot(int slot)
{
    return slot >= 0 && (size_t)slot < server_state.config.max_sessions;
}

/* Takes every waiting frame off a slot's queue into `out` (room for
 * LINK_SESSION_QUEUE_FRAMES), for freeing outside the lock. Call with
 * slots_lock held. Returns how many. */
static size_t take_queue_locked(slot_t *slot, outgoing_t **out)
{
    const size_t count = slot->queue_count;
    for (size_t i = 0; i < count; i++) {
        out[i] = slot->queue[(slot->queue_head + i) % LINK_SESSION_QUEUE_FRAMES];
    }
    slot->queue_head = 0;
    slot->queue_count = 0;
    return count;
}

static void free_frames(outgoing_t **frames, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(frames[i]);
    }
}

void http_server_release_slot(int slot)
{
    if (!valid_slot(slot)) {
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
    outgoing_t *unsent[LINK_SESSION_QUEUE_FRAMES];
    size_t unsent_count = 0;

    uint8_t *partial = NULL;
    taskENTER_CRITICAL(&server_state.slots_lock);
    const int slot = find_slot_locked(fd);
    if (slot >= 0) {
        slot_t *entry = &server_state.slots[slot];
        entry->state = SLOT_CLOSING;
        unsent_count = take_queue_locked(entry, unsent);
        partial = entry->partial;
        entry->partial = NULL;
    }
    taskEXIT_CRITICAL(&server_state.slots_lock);

    free_frames(unsent, unsent_count);
    free(partial);
    if (slot >= 0) {
        server_state.config.handlers.session_closed(server_state.config.handlers.context, slot);
    }
    close(fd);
}

/* ---------------------------------------------------------------------------
 * Sending
 *
 * Each session has its own queue. One job on the server's task,
 * send_pump(), moves frames to the sockets:
 *
 *   - A session's waiting frames are encoded into one batch and written
 *     together, which keeps the TCP stack from drowning in tiny writes when
 *     meters are polled fast.
 *   - Writes never wait. A socket takes what fits; the rest of the batch
 *     waits for the next pass, and the pump moves on to the next session.
 *     A client that reads slowly, or sits on a weak Wi-Fi link, therefore
 *     falls behind on its own: its queue fills, the hub drops its
 *     notifications and owes it a RESYNC, and after
 *     LINK_UNWRITABLE_TIMEOUT_MS without progress it is closed (bridge
 *     spec 8.2). Nobody else waits for it.
 *   - The pump runs until a pass moves nothing. http_server_service(),
 *     called every hub tick, starts it again for anything still waiting.
 * ------------------------------------------------------------------------- */

/* Most bytes one batch carries, and most bytes one run of the pump sends
 * before giving the server back its task for incoming frames. */
#define BATCH_MAX_BYTES 16384
#define PUMP_BYTES_PER_RUN 32768

static void send_pump(void *argument);

/* Asks the socket to close, once. Call with slots_lock held; returns the fd
 * to close outside the lock, or -1 if a close was already asked for. */
static int request_close_locked(slot_t *slot)
{
    if (slot->state != SLOT_OPEN || slot->close_requested) {
        return -1;
    }
    slot->close_requested = true;
    slot->close_queued = true;
    return slot->fd;
}

static void trigger_close(int fd)
{
    if (fd >= 0) {
        httpd_sess_trigger_close(server_state.link_server, fd);
    }
}

/* Queues send_pump() unless it already is. Call without slots_lock. */
static void schedule_pump(void)
{
    taskENTER_CRITICAL(&server_state.slots_lock);
    const bool needed = !server_state.pump_scheduled;
    server_state.pump_scheduled = true;
    taskEXIT_CRITICAL(&server_state.slots_lock);

    if (needed && httpd_queue_work(server_state.link_server, send_pump, NULL) != ESP_OK) {
        /* The work queue is full; http_server_service() tries again soon. */
        taskENTER_CRITICAL(&server_state.slots_lock);
        server_state.pump_scheduled = false;
        taskEXIT_CRITICAL(&server_state.slots_lock);
    }
}

/* Encodes a slot's waiting frames, up to BATCH_MAX_BYTES, as its new
 * partial batch. False if nothing waits or memory ran out. */
static bool start_batch(slot_t *slot)
{
    outgoing_t *frames[LINK_SESSION_QUEUE_FRAMES];
    size_t count = 0;
    size_t bytes = 0;

    taskENTER_CRITICAL(&server_state.slots_lock);
    while (slot->queue_count > 0) {
        outgoing_t *frame = slot->queue[slot->queue_head];
        const size_t size = WS_HEADER_MAX_BYTES + frame->length;
        if (count > 0 && bytes + size > BATCH_MAX_BYTES) {
            break; /* The rest go in the next batch */
        }
        frames[count++] = frame;
        bytes += size;
        slot->queue_head = (slot->queue_head + 1) % LINK_SESSION_QUEUE_FRAMES;
        slot->queue_count--;
    }
    taskEXIT_CRITICAL(&server_state.slots_lock);
    if (count == 0) {
        return false;
    }

    uint8_t *buffer = malloc(bytes);
    size_t length = 0;
    bool ends_with_close = false;
    for (size_t i = 0; i < count; i++) {
        if (buffer != NULL) {
            length += ws_header_encode(frames[i]->opcode, frames[i]->length, buffer + length);
            memcpy(buffer + length, frames[i]->data, frames[i]->length);
            length += frames[i]->length;
        }
        ends_with_close = frames[i]->opcode == WS_OPCODE_CLOSE;
        free(frames[i]);
    }
    if (buffer == NULL) {
        return false; /* The frames are lost; the hub's sequence gaps show it */
    }

    taskENTER_CRITICAL(&server_state.slots_lock);
    slot->partial = buffer;
    slot->partial_length = length;
    slot->partial_sent = 0;
    slot->partial_ends_with_close = ends_with_close;
    taskEXIT_CRITICAL(&server_state.slots_lock);
    return true;
}

/* The session's socket failed, so its stream is broken: drop what waits and
 * close it. */
static void fail_session(slot_t *slot)
{
    outgoing_t *unsent[LINK_SESSION_QUEUE_FRAMES];
    taskENTER_CRITICAL(&server_state.slots_lock);
    const size_t unsent_count = take_queue_locked(slot, unsent);
    uint8_t *partial = slot->partial;
    slot->partial = NULL;
    const int fd = request_close_locked(slot);
    taskEXIT_CRITICAL(&server_state.slots_lock);

    free_frames(unsent, unsent_count);
    free(partial);
    trigger_close(fd);
}

/* Sends as much of one session's output as its socket takes now. Returns
 * the bytes sent. */
static size_t service_slot(slot_t *slot)
{
    taskENTER_CRITICAL(&server_state.slots_lock);
    const bool sendable = slot->state == SLOT_OPEN && !slot->close_requested;
    const int fd = slot->fd;
    taskEXIT_CRITICAL(&server_state.slots_lock);
    if (!sendable || (slot->partial == NULL && !start_batch(slot))) {
        return 0;
    }

    /* The socket's own send, not httpd_socket_send(): that wrapper logs a
     * warning for every full socket, which here is routine back-pressure. */
    const ssize_t result = send(fd, slot->partial + slot->partial_sent,
                                slot->partial_length - slot->partial_sent, MSG_DONTWAIT);
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0; /* The socket is full: try again on the next pass */
    }
    if (result <= 0) {
        ESP_LOGW(TAG, "A session's socket failed (errno %d); closing it", errno);
        fail_session(slot);
        return 0;
    }

    int close_fd = -1;
    uint8_t *finished = NULL;
    taskENTER_CRITICAL(&server_state.slots_lock);
    slot->last_progress_us = esp_timer_get_time();
    slot->partial_sent += (size_t)result;
    if (slot->partial_sent == slot->partial_length) {
        finished = slot->partial;
        slot->partial = NULL;
        if (slot->partial_ends_with_close) {
            close_fd = request_close_locked(slot); /* The close frame has gone */
        }
    }
    taskEXIT_CRITICAL(&server_state.slots_lock);
    free(finished);
    trigger_close(close_fd);
    return (size_t)result;
}

/* Runs on the server's task. */
static void send_pump(void *argument)
{
    (void)argument;
    size_t sent_this_run = 0;
    for (;;) {
        size_t sent_this_pass = 0;
        for (size_t i = 0; i < server_state.config.max_sessions; i++) {
            sent_this_pass += service_slot(&server_state.slots[i]);
        }
        sent_this_run += sent_this_pass;

        const bool idle = sent_this_pass == 0;
        if (idle || sent_this_run >= PUMP_BYTES_PER_RUN) {
            taskENTER_CRITICAL(&server_state.slots_lock);
            server_state.pump_scheduled = false;
            taskEXIT_CRITICAL(&server_state.slots_lock);
            if (!idle) {
                schedule_pump(); /* Give the task back for incoming frames, then carry on */
            }
            return;
        }
    }
}

/* Queues one frame of any type for an open slot, within its queue limit. */
static bool queue_frame(int slot, uint8_t opcode, const uint8_t *data, size_t length)
{
    if (!valid_slot(slot)) {
        return false;
    }
    outgoing_t *frame = malloc(sizeof(outgoing_t) + length);
    if (frame == NULL) {
        return false;
    }
    frame->opcode = opcode;
    frame->length = length;
    if (length > 0) {
        memcpy(frame->data, data, length);
    }

    taskENTER_CRITICAL(&server_state.slots_lock);
    slot_t *entry = &server_state.slots[slot];
    const bool accepted = entry->state == SLOT_OPEN && !entry->close_queued &&
                          entry->queue_count < LINK_SESSION_QUEUE_FRAMES;
    if (accepted) {
        if (entry->queue_count == 0) {
            entry->last_progress_us = esp_timer_get_time();
        }
        entry->queue[(entry->queue_head + entry->queue_count) % LINK_SESSION_QUEUE_FRAMES] = frame;
        entry->queue_count++;
        entry->close_queued = opcode == WS_OPCODE_CLOSE;
    }
    taskEXIT_CRITICAL(&server_state.slots_lock);

    if (!accepted) {
        free(frame);
        return false;
    }
    schedule_pump();
    return true;
}

bool http_server_send(int slot, bool binary, const uint8_t *data, size_t length)
{
    return queue_frame(slot, binary ? WS_OPCODE_BINARY : WS_OPCODE_TEXT, data, length);
}

void http_server_ping(int slot)
{
    queue_frame(slot, WS_OPCODE_PING, NULL, 0);
}

void http_server_close(int slot, uint16_t code)
{
    /* The close frame waits its turn behind the frames already queued; the
     * pump closes the socket once it has gone out. */
    const uint8_t payload[2] = {(uint8_t)(code >> 8), (uint8_t)(code & 0xFF)};
    if (queue_frame(slot, WS_OPCODE_CLOSE, payload, sizeof(payload)) || !valid_slot(slot)) {
        return;
    }
    /* The queue is full or closing already: close without the frame. */
    taskENTER_CRITICAL(&server_state.slots_lock);
    const int fd = request_close_locked(&server_state.slots[slot]);
    taskEXIT_CRITICAL(&server_state.slots_lock);
    trigger_close(fd);
}

void http_server_service(void)
{
    const int64_t now_us = esp_timer_get_time();
    const int64_t limit_us = (int64_t)LINK_UNWRITABLE_TIMEOUT_MS * 1000;
    bool anything_waiting = false;

    for (size_t i = 0; i < server_state.config.max_sessions; i++) {
        int close_fd = -1;
        taskENTER_CRITICAL(&server_state.slots_lock);
        slot_t *slot = &server_state.slots[i];
        const bool waiting =
            slot->state == SLOT_OPEN && (slot->queue_count > 0 || slot->partial != NULL);
        if (waiting && now_us - slot->last_progress_us >= limit_us) {
            close_fd = request_close_locked(slot);
        }
        taskEXIT_CRITICAL(&server_state.slots_lock);

        anything_waiting = anything_waiting || waiting;
        if (close_fd >= 0) {
            ESP_LOGW(TAG, "Session %u has sent nothing for %d ms; closing it", (unsigned)i,
                     LINK_UNWRITABLE_TIMEOUT_MS);
            trigger_close(close_fd);
        }
    }
    if (anything_waiting) {
        schedule_pump(); /* In case an earlier attempt found the work queue full */
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
    const int fd = httpd_req_to_sockfd(request);
    const int slot = open_slot(fd);
    if (slot < 0) {
        return ESP_FAIL; /* The server closes the socket */
    }
    /* Frames go out in batches already; Nagle's delay would only hold a
     * meter frame back waiting for the client's acknowledgement. */
    const int no_delay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
    handlers->session_opened(handlers->context, slot);
    return ESP_OK;
}

/* Answers a client's control frame: a pong for a ping, a close for a close. */
/* Answers a client's control frame: a pong for a ping, a close for a
 * close. The answers join the session's queue like any frame, because
 * writing straight to the socket could land in the middle of a batch the
 * pump is part way through. */
static void answer_control_frame(int slot, const httpd_ws_frame_t *frame)
{
    const http_server_handlers_t *handlers = &server_state.config.handlers;
    if (frame->type == HTTPD_WS_TYPE_PING) {
        queue_frame(slot, WS_OPCODE_PONG, frame->payload, frame->len);
        handlers->session_activity(handlers->context, slot);
    } else if (frame->type == HTTPD_WS_TYPE_PONG) {
        handlers->session_activity(handlers->context, slot);
    } else if (frame->type == HTTPD_WS_TYPE_CLOSE) {
        /* Echo the client's close code; the pump closes once it has gone. */
        if (!queue_frame(slot, WS_OPCODE_CLOSE, frame->payload, frame->len >= 2 ? 2 : 0)) {
            http_server_close(slot, 1000);
        }
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
    answer_control_frame(slot, &frame);
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

/* Port 80. From the setup network, the setup page and API; from the home
 * network, a browser that typed the plain hostname goes to the Link port
 * (bridge spec 8.8). */
static esp_err_t on_port_80(httpd_req_t *request)
{
    if (server_state.config.setup_request != NULL &&
        net_socket_is_on_setup_network(httpd_req_to_sockfd(request))) {
        return server_state.config.setup_request(request);
    }
    if (request->method != HTTP_GET) {
        return httpd_resp_send_err(request, HTTPD_405_METHOD_NOT_ALLOWED, NULL);
    }

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
    /* A phone's captive-portal browser opens several connections at once and
     * keeps them open; when all are taken, drop the idlest rather than stall
     * the next request. No WebSocket lives on this port, so that is safe. */
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    config.stack_size = SERVER_STACK_BYTES;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&server_state.redirect_server, &config);
    if (err != ESP_OK) {
        return err;
    }
    const httpd_uri_t get = {.uri = "/*", .method = HTTP_GET, .handler = on_port_80};
    const httpd_uri_t post = {.uri = "/*", .method = HTTP_POST, .handler = on_port_80};
    err = httpd_register_uri_handler(server_state.redirect_server, &get);
    return err == ESP_OK ? httpd_register_uri_handler(server_state.redirect_server, &post) : err;
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
