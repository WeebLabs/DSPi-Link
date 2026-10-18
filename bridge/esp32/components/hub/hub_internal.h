/*
 * hub_internal.h - functions the hub's source files share. Not part of the
 * public API (hub.h).
 *
 * The hub is split by subject so each file can be read on its own:
 *
 *   hub.c           sessions, the message dispatch table, hello, keepalive
 *   hub_commands.c  CMD frames, the queue of requests to the uart task,
 *                   origin attribution
 *   hub_device.c    the device record and its events, notifications,
 *                   snapshots, locks, names
 *   hub_polls.c     poll subscriptions and their schedule
 */

#ifndef HUB_INTERNAL_H
#define HUB_INTERNAL_H

#include "cJSON.h"
#include "hub.h"

/* ---------------------------------------------------------------------------
 * hub.c
 * ------------------------------------------------------------------------- */

uint32_t hub_now(const hub_t *hub);

/* The session on `slot` if it has said hello, else NULL. */
hub_session_t *hub_open_session(hub_t *hub, int slot);

/* Sends `message` to one session and deletes it. Returns false if the
 * session's queue was full. */
bool hub_send_json(hub_t *hub, int slot, cJSON *message);

/* Sends `message` to every open session and deletes it. */
void hub_broadcast_json(hub_t *hub, cJSON *message);

/* Sends a binary frame to one session. */
bool hub_send_binary(hub_t *hub, int slot, const uint8_t *data, size_t length);

/* Replies {"t":"ok","id":id, ...body}. Takes ownership of `body`, which may
 * be NULL for an empty reply. */
void hub_reply_ok(hub_t *hub, int slot, int id, cJSON *body);

/* Replies {"t":"err","id":id,"code":code,"msg":msg}. */
void hub_reply_error(hub_t *hub, int slot, int id, const char *code, const char *msg);

/* Reads an integer member. False if absent or not a number. */
bool hub_json_int(const cJSON *object, const char *name, int *value);

/* ---------------------------------------------------------------------------
 * hub_commands.c
 * ------------------------------------------------------------------------- */

/* Handles one CMD request frame (Link spec 8.2, bridge spec 8.3). */
void hub_commands_handle_frame(hub_t *hub, int slot, const uint8_t *data, size_t length);

/* Queues a request the hub makes for itself (a poll or a snapshot read).
 * Returns false if the uart task's queue is full. */
bool hub_submit_own(hub_t *hub, hub_work_kind_t kind, const dspi_request_t *request, bool may_retry,
                    const hub_poll_key_t *poll);

/* The session that most likely caused a notification from `source`, or 0
 * (Link spec 8.3). */
uint16_t hub_take_origin(hub_t *hub, uint8_t source);

/* ---------------------------------------------------------------------------
 * hub_device.c
 * ------------------------------------------------------------------------- */

/* The device object of Link spec 7.4. */
cJSON *hub_device_json(const hub_t *hub);

/* True when handle 0 is a device that is online. */
bool hub_device_online(const hub_t *hub);

void hub_handle_device_list(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_device_rename(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_device_snapshot(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_lock_acquire(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_lock_release(hub_t *hub, int slot, int id, const cJSON *message);

/* A forwarded SET or a notification may have changed the settings blob. */
void hub_snapshot_mark_stale(hub_t *hub);

void hub_snapshot_on_reply(hub_t *hub, const hub_pending_t *work, const dspi_reply_t *reply);
void hub_device_session_closed(hub_t *hub, int slot, uint16_t session_id);
void hub_device_tick(hub_t *hub);

/* ---------------------------------------------------------------------------
 * hub_polls.c
 * ------------------------------------------------------------------------- */

void hub_handle_poll_subscribe(hub_t *hub, int slot, int id, const cJSON *message);
void hub_handle_poll_unsubscribe(hub_t *hub, int slot, int id, const cJSON *message);
void hub_polls_session_closed(hub_t *hub, int slot);

/* Ends every subscription: the device went offline (Link spec 7.6). */
void hub_polls_clear(hub_t *hub);

/* Starts the most overdue poll if the link is free. */
void hub_polls_tick(hub_t *hub);

void hub_polls_on_reply(hub_t *hub, const hub_pending_t *work, const dspi_reply_t *reply);

#endif /* HUB_INTERNAL_H */
