/*
 * http_server.h - the bridge's two HTTP servers: the Link port and port 80.
 *
 * Implements bridge spec 8.2 and 8.8 and Link spec 3.2, 4 and 4.2:
 *
 *   port 11915  /dspi/v1        the Link WebSocket (sub-protocol dspi-link-1)
 *               /dspi/v1/info   the discovery document
 *               everything else the web pages (web_assets)
 *   port 80     on the setup network: the setup page and its API
 *               on the home network: a redirect to port 11915
 *
 * Sessions are numbered by slot. A slot's life is FREE, then OPEN at the
 * WebSocket handshake, then CLOSING when the socket closes, then FREE again
 * only when the hub calls http_server_release_slot(). Holding the slot until
 * then means a new connection can never receive frames the hub meant for
 * the old one.
 *
 * Events reach the hub through the handlers in http_server_config_t, which
 * run on the HTTP server's task. Each function below says which task may
 * call it.
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef struct {
    void *context; /* Passed first to every handler */

    /* A session began, from `address` (IPv4, network byte order; 0 if not
     * known or the client uses IPv6), which the hub uses for rate limits. */
    void (*session_opened)(void *context, int slot, uint32_t address);

    /* A text or binary message. The handler takes ownership of `data`, a
     * heap buffer with a NUL after the last byte, and frees it with free(). */
    void (*session_message)(void *context, int slot, bool binary, uint8_t *data, size_t length);

    /* A ping or pong arrived: the client is alive. */
    void (*session_activity)(void *context, int slot);

    /* The session's socket has closed. The slot stays reserved until
     * http_server_release_slot(). */
    void (*session_closed)(void *context, int slot);

    /* Returns the /dspi/v1/info document as a heap string the server frees,
     * or NULL. */
    char *(*info_json)(void *context);
} http_server_handlers_t;

typedef struct {
    size_t max_sessions; /* At most LINK_MAX_SESSIONS_WITH_PSRAM */
    http_server_handlers_t handlers;

    /* Answers any request that reaches port 80 from the setup network
     * (setup_api.h). Requests from the home network get the redirect. */
    esp_err_t (*setup_request)(httpd_req_t *request);
} http_server_config_t;

/* Starts both servers. They listen on every interface from the start, so
 * they are ready whenever Wi-Fi connects. Called once, by app_main. Returns
 * ESP_OK, or the error that stopped a server starting. */
esp_err_t http_server_start(const http_server_config_t *config);

/* Queues one WebSocket message; the bytes are copied. False when the slot is
 * not open, is closing, already has LINK_SESSION_QUEUE_FRAMES messages
 * waiting (bridge spec 8.2), or memory ran out. Any task. */
bool http_server_send(int slot, bool binary, const uint8_t *data, size_t length);

/* The IPv4 address (network byte order) of the client on socket `fd`, or 0
 * if it is not known or the client uses IPv6. Any task, while the socket is
 * open: the HTTP server's handlers. */
uint32_t http_server_peer_address(int fd);

/* Queues a WebSocket ping. False, as for http_server_send(), when it cannot
 * be queued. Any task. */
bool http_server_ping(int slot);

/* Sends a close frame with `code` (Link spec 5) after the frames already
 * queued, then closes the socket; without the frame if the queue is full.
 * The close is retried until it happens. Any task. */
void http_server_close(int slot, uint16_t code);

/* The hub has finished with a closed session; the slot may be reused. Hub
 * task only, after session_closed. */
void http_server_release_slot(int slot);

/* Keeps the send queues moving: restarts sending, and closing, wherever an
 * earlier attempt found ESP-IDF's work queue full, and closes any session
 * that has sent nothing for LINK_UNWRITABLE_TIMEOUT_MS while frames wait.
 * Hub task, every hub tick. */
void http_server_service(void);

#endif /* HTTP_SERVER_H */
