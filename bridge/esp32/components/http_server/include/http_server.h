/*
 * http_server.h - the bridge's two HTTP servers: the Link port and port 80.
 *
 * Implements bridge spec 8.2 and 8.8 and Link spec 3.2, 4 and 4.2:
 *
 *   port 11915  /dspi/v1        the Link WebSocket (sub-protocol dspi-link-1)
 *               /dspi/v1/info   the discovery document
 *               everything else the web pages (web_assets)
 *   port 80     a redirect to port 11915 (the setup page joins in phase 4)
 *
 * Sessions are numbered by slot. A slot's life is FREE, then OPEN at the
 * WebSocket handshake, then CLOSING when the socket closes, then FREE again
 * only when the hub calls http_server_release_slot(). Holding the slot until
 * then means a new connection can never receive frames the hub meant for
 * the old one.
 *
 * Events reach the hub through the handlers in http_server_config_t, which
 * run on the HTTP server's task. The send, ping and close functions may be
 * called from any task.
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    void *context; /* Passed first to every handler */

    void (*session_opened)(void *context, int slot);

    /* A text or binary message. The handler takes ownership of `data`, a
     * heap buffer with a NUL after the last byte, and frees it with free(). */
    void (*session_message)(void *context, int slot, bool binary, uint8_t *data, size_t length);

    /* A ping or pong arrived: the client is alive. */
    void (*session_activity)(void *context, int slot);

    void (*session_closed)(void *context, int slot);

    /* Returns the /dspi/v1/info document as a heap string the server frees,
     * or NULL. */
    char *(*info_json)(void *context);
} http_server_handlers_t;

typedef struct {
    size_t max_sessions; /* At most LINK_MAX_SESSIONS_WITH_PSRAM */
    http_server_handlers_t handlers;
} http_server_config_t;

/* Starts both servers. They listen on every interface from the start, so
 * they are ready whenever Wi-Fi connects. */
esp_err_t http_server_start(const http_server_config_t *config);

/* Queues one WebSocket message; the bytes are copied. False when the slot is
 * not open or already has LINK_SESSION_QUEUE_FRAMES messages waiting. */
bool http_server_send(int slot, bool binary, const uint8_t *data, size_t length);

/* Sends a WebSocket ping. */
void http_server_ping(int slot);

/* Sends a close frame with `code` (Link spec 5), then closes the socket. */
void http_server_close(int slot, uint16_t code);

/* The hub has finished with a closed session; the slot may be reused. */
void http_server_release_slot(int slot);

/* Closes, with code 1011, any session whose socket has accepted nothing for
 * LINK_UNWRITABLE_TIMEOUT_MS while messages wait (Link spec 5). Call
 * regularly. */
void http_server_close_stalled(void);

#endif /* HTTP_SERVER_H */
