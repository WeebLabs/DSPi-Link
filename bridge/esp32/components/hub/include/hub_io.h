/*
 * hub_io.h - everything the hub reaches outside itself.
 *
 * The hub is a portable state machine (bridge spec 6.2). It never touches a
 * socket, the UART, flash or a clock directly; it calls these functions,
 * which hub_task.c supplies on the ESP32 and the host tests supply as
 * fakes. Every function receives `context` first.
 *
 * Sessions are numbered by slot, 0 to max_sessions - 1. The network side
 * owns the mapping from slots to sockets.
 */

#ifndef HUB_IO_H
#define HUB_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "dspi_frame.h"

typedef struct {
    void *context;

    /* Queues one WebSocket message for a session and returns at once. The
     * bytes are copied. Returns false when the session's outbound queue is
     * full (bridge spec 8.2), in which case nothing was queued. */
    bool (*send_text)(void *context, int slot, const char *text, size_t length);
    bool (*send_binary)(void *context, int slot, const uint8_t *data, size_t length);

    /* Sends a WebSocket ping (Link spec 4). */
    void (*ping)(void *context, int slot);

    /* Closes a session's socket with a WebSocket close code (Link spec 5).
     * The network side later reports it with hub_session_closed(). */
    void (*close)(void *context, int slot, uint16_t code);

    /* Queues a request for the uart task; the answer comes back through
     * hub_reply() with `tag`. A SET's payload must stay valid until then.
     * Returns false if the queue is full. */
    bool (*submit)(void *context, const dspi_request_t *request, bool may_retry, uint32_t tag);

    /* Milliseconds from any fixed point; wraps after 49 days. */
    uint32_t (*now_ms)(void *context);

    /* Saves the hub's name after hub.rename (bridge spec 8.11). */
    void (*save_hub_name)(void *context, const char *name);

    /* Saves and loads device names by serial (bridge spec 8.7). load returns
     * false when no name is saved. */
    void (*save_device_name)(void *context, const char *serial, const char *name);
    bool (*load_device_name)(void *context, const char *serial, char *name, size_t size);

    /* The hub's public description changed (name, devices): refresh the
     * DNS-SD TXT record (bridge spec 8.8). */
    void (*advertise)(void *context);

    /* Adds the `bridge` object's fields to a hub.stats reply (bridge spec
     * 8.7). May be NULL. */
    void (*describe_bridge)(void *context, cJSON *bridge);
} hub_io_t;

#endif /* HUB_IO_H */
