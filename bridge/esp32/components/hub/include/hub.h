/*
 * hub.h - the DSPi Link hub: sessions, commands, notifications, polls,
 * snapshots and locks for the one DSPi behind the bridge.
 *
 * Implements Link spec 5 to 8 as bridge spec 8 describes. The hub is a plain
 * state machine. Something calls one hub function per event (a WebSocket
 * message, a UART reply, a timer tick) and the hub reacts through hub_io_t.
 * On the ESP32 that something is the hub task (hub_task.h), which is the
 * only caller, so nothing here needs locking. The host tests call the same
 * functions directly.
 *
 * Portable: no ESP-IDF headers. Phase 3 implements `auth: none` only; the
 * PIN and the administration messages arrive in phase 5.
 */

#ifndef HUB_H
#define HUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_config.h"
#include "dspi_link.h"
#include "hub_io.h"
#include "link_frames.h"
#include "link_policy.h"
#include "settings_model.h"

/* The bridge serves one DSPi, always at handle 0 (bridge spec 1). */
#define HUB_DEVICE_HANDLE 0

#define HUB_MAX_SESSIONS LINK_MAX_SESSIONS_WITH_PSRAM
#define HUB_POLL_SLOTS 16      /* Slots 0 to 15 per session (Link spec 7.6) */
#define HUB_MAX_POLL_GROUPS 32 /* Distinct polls across all sessions */
#define HUB_ORIGIN_QUEUE 16    /* Recent SETs awaiting a notification (Link spec 8.3) */
#define HUB_PENDING_CAPACITY DSPI_UART_QUEUE_DEPTH

/* How often hub_tick() must be called. */
#define HUB_TICK_MS 10

/* ---------------------------------------------------------------------------
 * State. Treat every type below as opaque; they are public only so a hub
 * can live in static storage.
 * ------------------------------------------------------------------------- */

typedef enum {
    HUB_SESSION_FREE,
    HUB_SESSION_AWAITING_HELLO,
    HUB_SESSION_OPEN,
    HUB_SESSION_CLOSING, /* Close requested; waiting for hub_session_closed() */
} hub_session_state_t;

/* What one poll asks the DSPi. Identical keys from several sessions run
 * once (Link spec 7.6). */
typedef struct {
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} hub_poll_key_t;

typedef struct {
    bool active;
    hub_poll_key_t key;
    uint8_t hz;            /* The granted rate */
    uint32_t last_sent_ms; /* So a faster group never exceeds that rate */
} hub_poll_slot_t;

typedef struct {
    hub_session_state_t state;
    uint16_t id; /* Link session id, never 0 */
    char client_name[64];
    link_role_t role;
    uint32_t opened_ms;
    uint32_t last_activity_ms; /* Any frame from the client, pongs included */
    uint32_t last_ping_ms;
    uint8_t commands_in_flight;
    uint16_t notify_sequence;
    uint16_t poll_sequence;
    bool resync_owed; /* A notification was dropped; RESYNC when there is room */
    hub_poll_slot_t polls[HUB_POLL_SLOTS];
} hub_session_t;

/* What a request sent to the uart task was for. */
typedef enum {
    HUB_WORK_CLIENT_COMMAND,
    HUB_WORK_POLL,
    HUB_WORK_SNAPSHOT_BULK,
    HUB_WORK_SNAPSHOT_STATUS,
} hub_work_kind_t;

/* One request waiting for the uart task. The uart task answers in order,
 * so these form a queue. */
typedef struct {
    uint32_t tag;
    hub_work_kind_t kind;
    int slot;            /* Client command: who sent it */
    uint16_t session_id; /* Client command: detects a slot reused meanwhile */
    uint16_t client_tag; /* Client command: the CMD frame's tag */
    bool is_set;         /* Client command: a SET, for origin attribution */
    bool is_bulk_set;    /* Client command: its payload is in bulk_set_buffer */
    hub_poll_key_t poll; /* Poll: which one */
    uint32_t submitted_ms;
    uint8_t payload[DSPI_UART_MAX_SET_PAYLOAD_BYTES]; /* A small SET's payload */
} hub_pending_t;

typedef struct {
    hub_poll_key_t key;
    uint32_t period_ms;
    uint32_t next_due_ms;
    uint8_t failures_in_a_row;
} hub_poll_group_t;

typedef enum {
    HUB_DEVICE_ABSENT,
    HUB_DEVICE_ONLINE,
    HUB_DEVICE_OFFLINE, /* Known but not answering; handle kept for 60 s */
} hub_device_state_t;

typedef struct {
    hub_device_state_t state;
    dspi_identity_t identity;
    char name[SETTINGS_NAME_MAX_BYTES + 1];
    uint32_t baud;
    uint32_t offline_since_ms;
    bool header_known; /* The bulk header below has been read */
    uint8_t wire_version;
    uint8_t channels;
    uint8_t inputs;
    bool seq_known;
    uint8_t last_seq;   /* Of the last notification, to spot gaps */
    uint16_t locked_by; /* Session id, or 0 */
    uint32_t lock_expires_ms;
    uint32_t commands;
    uint32_t errors;
    uint32_t average_rtt_ms;
    uint32_t notifications_dropped;
} hub_device_t;

typedef struct {
    int slot;
    uint16_t session_id;
    int request_id;
} hub_snapshot_waiter_t;

typedef struct {
    uint8_t bulk[LINK_MAX_PAYLOAD_BYTES];
    uint16_t bulk_length;
    uint8_t status[DSPI_UART_MAX_GET_REPLY_BYTES];
    uint16_t status_length;
    bool valid;      /* bulk and status hold a complete read */
    bool stale;      /* Something may have changed since */
    bool refreshing; /* A read is in progress */
    uint32_t read_ms;
    hub_snapshot_waiter_t waiters[HUB_MAX_SESSIONS];
    size_t waiter_count;
} hub_snapshot_t;

typedef struct {
    uint16_t session_id;
    uint32_t sent_ms;
} hub_origin_t;

typedef struct {
    char hub_id[SETTINGS_HUB_ID_CHARS + 1];
    char name[SETTINGS_NAME_MAX_BYTES + 1];
    const char *version; /* The bridge firmware's version */
    size_t max_sessions; /* At most HUB_MAX_SESSIONS */
} hub_config_t;

typedef struct {
    hub_io_t io;
    hub_config_t config;
    uint32_t started_ms;

    hub_session_t sessions[HUB_MAX_SESSIONS];
    uint16_t next_session_id;

    hub_pending_t pending[HUB_PENDING_CAPACITY];
    size_t pending_head;
    size_t pending_count;
    uint32_t next_tag;
    size_t client_commands_pending;
    bool poll_pending;

    uint8_t bulk_set_buffer[LINK_MAX_PAYLOAD_BYTES]; /* The one bulk SET in flight */
    bool bulk_set_busy;

    hub_origin_t origins[HUB_ORIGIN_QUEUE];
    size_t origin_head;
    size_t origin_count;

    hub_poll_group_t poll_groups[HUB_MAX_POLL_GROUPS];
    size_t poll_group_count;

    hub_device_t device;
    hub_snapshot_t snapshot;

    /* Scratch for encoding one outbound binary frame; the largest is a CMD
     * response carrying a whole bulk blob. */
    uint8_t frame[LINK_CMD_RESPONSE_HEADER_BYTES + LINK_MAX_PAYLOAD_BYTES];
} hub_t;

/* ---------------------------------------------------------------------------
 * Lifecycle and time
 * ------------------------------------------------------------------------- */

void hub_init(hub_t *hub, const hub_io_t *io, const hub_config_t *config);

/* Runs timers: keepalive, hello timeout, lock expiry, offline retention and
 * due polls. Call every HUB_TICK_MS. */
void hub_tick(hub_t *hub);

/* ---------------------------------------------------------------------------
 * Network events
 * ------------------------------------------------------------------------- */

/* True when another session may connect; checked before the WebSocket
 * handshake so a full bridge answers HTTP 503 (bridge spec 8.2). */
bool hub_has_room(const hub_t *hub);

/* A WebSocket session finished its handshake on `slot`. */
void hub_session_opened(hub_t *hub, int slot);

/* A text (JSON) or binary message arrived. */
void hub_session_text(hub_t *hub, int slot, const char *text, size_t length);
void hub_session_binary(hub_t *hub, int slot, const uint8_t *data, size_t length);

/* A pong or other control frame arrived: the client is alive. */
void hub_session_activity(hub_t *hub, int slot);

/* The socket closed, for whatever reason. */
void hub_session_closed(hub_t *hub, int slot);

/* ---------------------------------------------------------------------------
 * Device events, from the uart task
 * ------------------------------------------------------------------------- */

void hub_device_found(hub_t *hub, const dspi_identity_t *identity, uint32_t baud);
void hub_device_lost(hub_t *hub);
void hub_notification(hub_t *hub, const uint8_t *packet, size_t length);
void hub_reply(hub_t *hub, uint32_t tag, const dspi_reply_t *reply);

/* Renames the hub, as hub.rename does: saves the name and advertises it.
 * False if the name is not 1 to 63 bytes of UTF-8 (bridge spec 8.11). */
bool hub_rename(hub_t *hub, const char *name);

/* ---------------------------------------------------------------------------
 * What the hub publishes
 * ------------------------------------------------------------------------- */

/* The /dspi/v1/info document (Link spec 3.2). The caller frees it with
 * cJSON_free(). NULL if memory ran out. */
char *hub_info_json(const hub_t *hub);

/* The values the DNS-SD TXT record needs (Link spec 3.1). */
typedef struct {
    const char *hub_id;
    const char *name;
    const char *auth;   /* "none" or "pin" */
    unsigned devices;   /* `n` */
    const char *serial; /* `d`: the one device's serial, or NULL */
} hub_advert_t;

void hub_advert(const hub_t *hub, hub_advert_t *advert);

/* Poll budget in bytes per second at the current baud rate (bridge spec
 * 7.7). */
uint32_t hub_poll_budget(const hub_t *hub);

#endif /* HUB_H */
