/*
 * http_server.c - the Link port and port 80. See http_server.h; implements
 * bridge spec 8.2 and 8.8 and Link spec 3.2, 4 and 4.2.
 *
 * The slot table, with each session's queue of frames to send, is the only
 * state shared between tasks here: the HTTP server's task opens, closes and
 * sends, while the hub task queues frames and asks for closes. Every access
 * takes `slots_lock` for a few instructions; nothing is allocated or sent
 * while it is held.
 *
 * Only the server's task touches sockets. Another task that wants a session
 * closed marks its slot, and the send pump, on the server's task, makes the
 * request: there the fd cannot have been closed and reused under it, since
 * on_socket_closed() runs on that task too. ESP-IDF's control queue, which
 * carries both the pump and close requests, is small and can be full; a
 * refused request stays marked and http_server_service() tries again.
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
/* Room for the sub-protocols a client offers; a Link client offers one. */
#define SUBPROTOCOL_HEADER_BYTES 96
#define CLOSE_NORMAL 1000
#define CLOSE_PROTOCOL_ERROR 4000

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
    bool close_queued; /* A close frame is waiting; nothing more is accepted */

    /* Closing the socket: wanted (the pump is still to ask ESP-IDF), then
     * requested (ESP-IDF accepted, at `close_requested_us`). */
    bool close_wanted;
    bool close_requested;
    int64_t close_requested_us;

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
    bool work_added;     /* Frames or closes arrived during the pump's pass */
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
    /* With close_fn set, closing the socket is this function's job. */
    if (close(fd) != 0) {
        ESP_LOGW(TAG, "Closing socket %d failed (errno %d)", fd, errno);
    }
}

/* ---------------------------------------------------------------------------
 * Sending
 *
 * Each session has its own queue. One job on the server's task,
 * send_pump(), moves frames to the sockets and carries out closes:
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
 *   - The pump runs until a pass moves nothing and nothing new arrived
 *     during it. http_server_service(), called every hub tick, starts it
 *     again for anything still waiting, including closes ESP-IDF refused.
 * ------------------------------------------------------------------------- */

static void send_pump(void *argument);

/* Marks the slot's socket to be closed, once. Call with slots_lock held;
 * the pump carries the close out. */
static void request_close_locked(slot_t *slot)
{
    if (slot->state != SLOT_OPEN || slot->close_wanted) {
        return;
    }
    slot->close_wanted = true;
    slot->close_queued = true;
    server_state.work_added = true;
}

/* Queues send_pump() unless it already is. Call without slots_lock. */
static void schedule_pump(void)
{
    taskENTER_CRITICAL(&server_state.slots_lock);
    const bool needed = !server_state.pump_scheduled;
    server_state.pump_scheduled = true;
    taskEXIT_CRITICAL(&server_state.slots_lock);

    if (needed && httpd_queue_work(server_state.link_server, send_pump, NULL) != ESP_OK) {
        /* ESP-IDF's control queue is full (it has logged so); the next
         * http_server_service() tries again. */
        taskENTER_CRITICAL(&server_state.slots_lock);
        server_state.pump_scheduled = false;
        taskEXIT_CRITICAL(&server_state.slots_lock);
    }
}

/* Asks ESP-IDF to close a slot's socket, if that is wanted and not yet
 * asked, dropping whatever still waits. Server's task only, so the slot's
 * fd is still the session's own. A refused request stays wanted. */
static void carry_out_close(slot_t *slot)
{
    outgoing_t *unsent[LINK_SESSION_QUEUE_FRAMES];
    size_t unsent_count = 0;
    uint8_t *partial = NULL;

    taskENTER_CRITICAL(&server_state.slots_lock);
    const bool due = slot->state == SLOT_OPEN && slot->close_wanted && !slot->close_requested;
    const int fd = slot->fd;
    if (due) {
        unsent_count = take_queue_locked(slot, unsent);
        partial = slot->partial;
        slot->partial = NULL;
    }
    taskEXIT_CRITICAL(&server_state.slots_lock);

    free_frames(unsent, unsent_count);
    free(partial);
    if (!due) {
        return;
    }
    /* NOT_FOUND: ESP-IDF has already dropped the session itself. */
    const esp_err_t err = httpd_sess_trigger_close(server_state.link_server, fd);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGD(TAG, "Closing socket %d deferred: %s", fd, esp_err_to_name(err));
        return;
    }
    taskENTER_CRITICAL(&server_state.slots_lock);
    slot->close_requested = true;
    slot->close_requested_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&server_state.slots_lock);
}

/* Encodes a slot's waiting frames, up to HTTP_BATCH_MAX_BYTES, as its new
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
        if (count > 0 && bytes + size > HTTP_BATCH_MAX_BYTES) {
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
        /* The frames are lost; the hub's sequence gaps show it. A lost close
         * frame must still close the socket, or the session would hang. */
        ESP_LOGW(TAG, "No memory for %u bytes of a session's frames; dropped them",
                 (unsigned)bytes);
        if (ends_with_close) {
            taskENTER_CRITICAL(&server_state.slots_lock);
            request_close_locked(slot);
            taskEXIT_CRITICAL(&server_state.slots_lock);
        }
        return false;
    }

    taskENTER_CRITICAL(&server_state.slots_lock);
    slot->partial = buffer;
    slot->partial_length = length;
    slot->partial_sent = 0;
    slot->partial_ends_with_close = ends_with_close;
    taskEXIT_CRITICAL(&server_state.slots_lock);
    return true;
}

/* Sends as much of one session's output as its socket takes now, or closes
 * it if that is wanted. Returns the bytes sent. Server's task only. */
static size_t service_slot(slot_t *slot)
{
    taskENTER_CRITICAL(&server_state.slots_lock);
    const bool open = slot->state == SLOT_OPEN;
    const bool closing = slot->close_wanted;
    const int fd = slot->fd;
    taskEXIT_CRITICAL(&server_state.slots_lock);
    if (!open) {
        return 0;
    }
    if (closing || (slot->partial == NULL && !start_batch(slot))) {
        carry_out_close(slot); /* Does nothing unless a close is due */
        return 0;
    }

    /* The socket's own send, not httpd_socket_send(): that wrapper logs a
     * warning for every full socket, which here is routine back-pressure. */
    const ssize_t result = send(fd, slot->partial + slot->partial_sent,
                                slot->partial_length - slot->partial_sent, MSG_DONTWAIT);
    const int send_errno = errno;
    if (result < 0 && (send_errno == EAGAIN || send_errno == EWOULDBLOCK)) {
        return 0; /* The socket is full: try again on the next pass */
    }

    uint8_t *finished = NULL;
    taskENTER_CRITICAL(&server_state.slots_lock);
    if (result <= 0) {
        request_close_locked(slot); /* The stream is broken */
    } else {
        slot->last_progress_us = esp_timer_get_time();
        slot->partial_sent += (size_t)result;
        if (slot->partial_sent == slot->partial_length) {
            finished = slot->partial;
            slot->partial = NULL;
            if (slot->partial_ends_with_close) {
                request_close_locked(slot); /* The close frame has gone */
            }
        }
    }
    taskEXIT_CRITICAL(&server_state.slots_lock);

    free(finished);
    if (result <= 0) {
        ESP_LOGW(TAG, "A session's socket failed (errno %d); closing it", send_errno);
    }
    carry_out_close(slot);
    return result > 0 ? (size_t)result : 0;
}

/* Runs on the server's task. */
static void send_pump(void *argument)
{
    (void)argument;
    size_t sent_this_run = 0;
    for (;;) {
        taskENTER_CRITICAL(&server_state.slots_lock);
        server_state.work_added = false;
        taskEXIT_CRITICAL(&server_state.slots_lock);

        size_t sent_this_pass = 0;
        for (size_t i = 0; i < server_state.config.max_sessions; i++) {
            sent_this_pass += service_slot(&server_state.slots[i]);
        }
        sent_this_run += sent_this_pass;

        const bool idle = sent_this_pass == 0;
        const bool share_used = sent_this_run >= HTTP_PUMP_BYTES_PER_RUN;
        if (!idle && !share_used) {
            continue;
        }
        /* A frame queued during the pass, behind its slot, saw the pump
         * already scheduled and did not schedule it again: run once more. */
        taskENTER_CRITICAL(&server_state.slots_lock);
        const bool run_again = idle && server_state.work_added && !share_used;
        if (!run_again) {
            server_state.pump_scheduled = false;
        }
        taskEXIT_CRITICAL(&server_state.slots_lock);
        if (run_again) {
            continue;
        }
        if (!idle) {
            schedule_pump(); /* Give the task back for incoming frames, then carry on */
        }
        return;
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
        server_state.work_added = true;
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

bool http_server_ping(int slot)
{
    return queue_frame(slot, WS_OPCODE_PING, NULL, 0);
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
    request_close_locked(&server_state.slots[slot]);
    taskEXIT_CRITICAL(&server_state.slots_lock);
    schedule_pump();
}

void http_server_service(void)
{
    const int64_t now_us = esp_timer_get_time();
    const int64_t unwritable_us = (int64_t)LINK_UNWRITABLE_TIMEOUT_MS * 1000;
    const int64_t close_retry_us = (int64_t)HTTP_CLOSE_RETRY_MS * 1000;
    bool pump_needed = false;

    for (size_t i = 0; i < server_state.config.max_sessions; i++) {
        taskENTER_CRITICAL(&server_state.slots_lock);
        slot_t *slot = &server_state.slots[i];
        const bool open = slot->state == SLOT_OPEN;
        const bool waiting =
            open && !slot->close_wanted && (slot->queue_count > 0 || slot->partial != NULL);
        const bool stuck = waiting && now_us - slot->last_progress_us >= unwritable_us;
        if (stuck) {
            request_close_locked(slot);
        }
        /* ESP-IDF accepted a close but the socket is still open: ask again. */
        if (open && slot->close_requested && now_us - slot->close_requested_us >= close_retry_us) {
            slot->close_requested = false;
        }
        const bool close_due = open && slot->close_wanted && !slot->close_requested;
        taskEXIT_CRITICAL(&server_state.slots_lock);

        pump_needed = pump_needed || waiting || close_due;
        if (stuck) {
            ESP_LOGW(TAG, "Session %u has sent nothing for %d ms; closing it", (unsigned)i,
                     LINK_UNWRITABLE_TIMEOUT_MS);
        }
    }
    if (pump_needed) {
        schedule_pump(); /* In case an earlier attempt found the work queue full */
    }
}

/* ---------------------------------------------------------------------------
 * Replies
 * ------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *value; /* Must last until the reply is sent */
} header_t;

/* Sets a reply's status (NULL keeps 200 OK), content type (NULL keeps the
 * default) and headers, stopping at the first that fails, which it logs
 * and returns. */
static esp_err_t prepare_reply(httpd_req_t *request, const char *status, const char *type,
                               const header_t *headers, size_t count)
{
    esp_err_t err = status != NULL ? httpd_resp_set_status(request, status) : ESP_OK;
    if (err == ESP_OK && type != NULL) {
        err = httpd_resp_set_type(request, type);
    }
    for (size_t i = 0; i < count && err == ESP_OK; i++) {
        err = httpd_resp_set_hdr(request, headers[i].name, headers[i].value);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not prepare a reply to %s: %s", request->uri, esp_err_to_name(err));
    }
    return err;
}

/* ---------------------------------------------------------------------------
 * The Link WebSocket (Link spec 4)
 * ------------------------------------------------------------------------- */

/* True when the client asked for the Link sub-protocol among those it
 * offers (Link spec 4). ESP-IDF completes a handshake without one, so the
 * bridge checks for itself. */
static bool requests_link_subprotocol(httpd_req_t *request)
{
    char offered[SUBPROTOCOL_HEADER_BYTES];
    if (httpd_req_get_hdr_value_str(request, "Sec-WebSocket-Protocol", offered, sizeof(offered)) !=
        ESP_OK) {
        return false; /* None offered, or a list too long to be a Link client's */
    }
    char *rest = NULL;
    for (char *name = strtok_r(offered, ", ", &rest); name != NULL;
         name = strtok_r(NULL, ", ", &rest)) {
        if (strcmp(name, LINK_SUBPROTOCOL) == 0) {
            return true;
        }
    }
    return false;
}

/* Answers a handshake the bridge will not complete. The server closes the
 * socket once this returns ESP_FAIL. */
static esp_err_t refuse_handshake(httpd_req_t *request, const char *status, const char *text)
{
    if (prepare_reply(request, status, NULL, NULL, 0) == ESP_OK &&
        httpd_resp_sendstr(request, text) != ESP_OK) {
        ESP_LOGD(TAG, "Could not send the %s refusal", status);
    }
    return ESP_FAIL;
}

/* Before the handshake: a client that does not speak Link gets 400, and a
 * full bridge answers 503 (Link spec 4, bridge spec 8.2). */
static esp_err_t before_handshake(httpd_req_t *request)
{
    if (!requests_link_subprotocol(request)) {
        return refuse_handshake(request, "400 Bad Request",
                                "Link needs the " LINK_SUBPROTOCOL " sub-protocol.");
    }
    if (has_free_slot()) {
        return ESP_OK;
    }
    return refuse_handshake(request, "503 Service Unavailable",
                            "This bridge has no room for another session.");
}

/* The IPv4 address in `address`, network byte order, or 0 if it has none.
 * The servers listen on IPv6, so IPv4 clients appear as ::ffff:a.b.c.d; a
 * real IPv6 address has no IPv4 form, and its last four bytes are the
 * client's to choose. */
static uint32_t ipv4_of(const struct sockaddr_storage *address)
{
    static const uint8_t V4_MAPPED_PREFIX[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (address->ss_family == AF_INET) {
        return ((const struct sockaddr_in *)address)->sin_addr.s_addr;
    }
    if (address->ss_family != AF_INET6) {
        return 0;
    }
    const uint8_t *bytes = ((const struct sockaddr_in6 *)address)->sin6_addr.s6_addr;
    if (memcmp(bytes, V4_MAPPED_PREFIX, sizeof(V4_MAPPED_PREFIX)) != 0) {
        return 0;
    }
    uint32_t ipv4 = 0;
    memcpy(&ipv4, bytes + sizeof(V4_MAPPED_PREFIX), sizeof(ipv4));
    return ipv4;
}

uint32_t http_server_peer_address(int fd)
{
    struct sockaddr_storage peer;
    socklen_t length = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &length) != 0) {
        return 0;
    }
    return ipv4_of(&peer);
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
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) != 0) {
        ESP_LOGW(TAG, "Session %d keeps Nagle's delay (errno %d)", slot, errno);
    }
    handlers->session_opened(handlers->context, slot, http_server_peer_address(fd));
    return ESP_OK;
}

/* Answers a client's control frame: a pong for a ping, a close for a
 * close. The answers join the session's queue like any frame, because
 * writing straight to the socket could land in the middle of a batch the
 * pump is part way through. */
static void answer_control_frame(int slot, const httpd_ws_frame_t *frame)
{
    const http_server_handlers_t *handlers = &server_state.config.handlers;
    if (frame->type == HTTPD_WS_TYPE_PING) {
        if (!queue_frame(slot, WS_OPCODE_PONG, frame->payload, frame->len)) {
            ESP_LOGD(TAG, "Session %d: no room for a pong", slot); /* The next ping gets one */
        }
        handlers->session_activity(handlers->context, slot);
    } else if (frame->type == HTTPD_WS_TYPE_PONG) {
        handlers->session_activity(handlers->context, slot);
    } else if (frame->type == HTTPD_WS_TYPE_CLOSE) {
        /* Echo the client's close code; the pump closes once it has gone. */
        if (!queue_frame(slot, WS_OPCODE_CLOSE, frame->payload, frame->len >= 2 ? 2 : 0)) {
            http_server_close(slot, CLOSE_NORMAL);
        }
    }
}

/* Writes a close frame straight to a session's socket just before the
 * server drops it, so the client learns why. Only between batches, where it
 * cannot land inside one. Server's task only. */
static void send_close_now(int slot, uint16_t code)
{
    const slot_t *entry = &server_state.slots[slot];
    if (entry->partial != NULL) {
        return; /* Part of a batch has gone; a close now would corrupt it */
    }
    uint8_t frame[WS_HEADER_MAX_BYTES + 2];
    const size_t header = ws_header_encode(WS_OPCODE_CLOSE, 2, frame);
    frame[header] = (uint8_t)(code >> 8);
    frame[header + 1] = (uint8_t)(code & 0xFF);
    if (send(entry->fd, frame, header + 2, MSG_DONTWAIT) != (ssize_t)(header + 2)) {
        ESP_LOGD(TAG, "Session %d: the close frame did not go (errno %d)", slot, errno);
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
        /* Too big, or fragmented, which no Link client sends (Link spec 4).
         * The frame is not read, so the server must drop the socket now. */
        send_close_now(slot, CLOSE_PROTOCOL_ERROR);
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
    const header_t headers[] = {
        {"Access-Control-Allow-Origin", "*"},
        {"Cache-Control", "no-store"},
    };
    esp_err_t err = prepare_reply(request, NULL, "application/json", headers,
                                  sizeof(headers) / sizeof(headers[0]));
    if (err == ESP_OK) {
        err = httpd_resp_sendstr(request, json);
    }
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
        const esp_err_t err = prepare_reply(request, "304 Not Modified", NULL, NULL, 0);
        return err == ESP_OK ? httpd_resp_send(request, NULL, 0) : err;
    }

    /* Vite names assets by their hash, so those never change; pages may. */
    const header_t headers[] = {
        {"Content-Encoding", "gzip"},
        {"ETag", asset->etag},
        {"Cache-Control", asset->immutable ? "public, max-age=31536000, immutable" : "no-cache"},
    };
    const esp_err_t err = prepare_reply(request, NULL, asset->content_type, headers,
                                        sizeof(headers) / sizeof(headers[0]));
    return err == ESP_OK
               ? httpd_resp_send(request, (const char *)asset->data, (ssize_t)asset->length)
               : err;
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
    /* Drop the port; an IPv6 literal keeps the colons inside its brackets. */
    char *name_end = host[0] == '[' ? strchr(host, ']') : host;
    char *port = name_end != NULL ? strchr(name_end, ':') : NULL;
    if (port != NULL) {
        *port = '\0';
    }
    char location[96];
    const int written = snprintf(location, sizeof(location), "http://%s:%d/", host, LINK_TCP_PORT);
    if (written < 0 || (size_t)written >= sizeof(location)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Host header too long");
    }
    const header_t headers[] = {{"Location", location}};
    const esp_err_t err = prepare_reply(request, "302 Found", NULL, headers, 1);
    return err == ESP_OK ? httpd_resp_send(request, NULL, 0) : err;
}

/* ---------------------------------------------------------------------------
 * Start
 * ------------------------------------------------------------------------- */

static esp_err_t start_link_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = LINK_TCP_PORT;
    config.max_open_sockets = server_state.config.max_sessions + HTTP_LINK_EXTRA_SOCKETS;
    config.stack_size = HTTP_SERVER_STACK_BYTES;
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
    /* When every socket is taken, drop the idlest rather than stall the next
     * request. No WebSocket lives on this port, so that is safe. */
    config.max_open_sockets = HTTP_SETUP_MAX_SOCKETS;
    config.lru_purge_enable = true;
    config.stack_size = HTTP_SERVER_STACK_BYTES;
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
