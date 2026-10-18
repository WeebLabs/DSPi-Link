/*
 * hub_commands.c - CMD frames, the queue of requests to the uart task, and
 * origin attribution.
 *
 * Implements Link spec 8.2 and the origin rule of 8.3, as bridge spec 8.3
 * and 8.4 describe. Every request the hub sends the DSPi, whether a client's
 * command, a poll or a snapshot read, goes through one queue here. The uart
 * task answers in the order it was asked, so the oldest pending request is
 * always the one being answered.
 *
 * Portable: no ESP-IDF headers.
 */

#include <string.h>

#include "hub_internal.h"

/* REQ_SET_ALL_PARAMS: the bulk SET, which the UART carries in one frame and
 * the firmware accepts above the 64-byte limit for other SETs. */
#define REQ_SET_ALL_PARAMS 0xA1

/* Notification sources that a Link client's write produces (Link spec 8.3):
 * HOST_SET, BULK_SET and UART. */
#define SOURCE_HOST_SET 1
#define SOURCE_BULK_SET 2
#define SOURCE_UART 8

/* ---------------------------------------------------------------------------
 * The pending queue
 * ------------------------------------------------------------------------- */

/* The free entry at the back of the queue, or NULL if full. It joins the
 * queue only on commit_pending(), so a request the uart task refuses leaves
 * no trace. */
static hub_pending_t *reserve_pending(hub_t *hub)
{
    if (hub->pending_count == HUB_PENDING_CAPACITY) {
        return NULL;
    }
    const size_t back = (hub->pending_head + hub->pending_count) % HUB_PENDING_CAPACITY;
    hub_pending_t *entry = &hub->pending[back];
    memset(entry, 0, sizeof(*entry));
    entry->tag = hub->next_tag++;
    entry->submitted_ms = hub_now(hub);
    return entry;
}

static void commit_pending(hub_t *hub)
{
    hub->pending_count++;
}

/* Takes the oldest entry off the queue, copying it out so the caller may
 * queue new work while handling it. */
static void pop_pending(hub_t *hub, hub_pending_t *out)
{
    *out = hub->pending[hub->pending_head];
    hub->pending_head = (hub->pending_head + 1) % HUB_PENDING_CAPACITY;
    hub->pending_count--;
}

bool hub_submit_own(hub_t *hub, hub_work_kind_t kind, const dspi_request_t *request, bool may_retry,
                    const hub_poll_key_t *poll)
{
    hub_pending_t *entry = reserve_pending(hub);
    if (entry == NULL) {
        return false;
    }
    entry->kind = kind;
    if (poll != NULL) {
        entry->poll = *poll;
    }
    if (!hub->io.submit(hub->io.context, request, may_retry, entry->tag)) {
        return false;
    }
    commit_pending(hub);
    return true;
}

/* ---------------------------------------------------------------------------
 * Origin attribution (Link spec 8.3)
 * ------------------------------------------------------------------------- */

static void remember_write(hub_t *hub, uint16_t session_id)
{
    if (hub->origin_count == HUB_ORIGIN_QUEUE) {
        /* Full: forget the oldest write, which is the least likely to match. */
        hub->origin_head = (hub->origin_head + 1) % HUB_ORIGIN_QUEUE;
        hub->origin_count--;
    }
    const size_t back = (hub->origin_head + hub->origin_count) % HUB_ORIGIN_QUEUE;
    hub->origins[back] = (hub_origin_t){.session_id = session_id, .sent_ms = hub_now(hub)};
    hub->origin_count++;
}

uint16_t hub_take_origin(hub_t *hub, uint8_t source)
{
    if (source != SOURCE_HOST_SET && source != SOURCE_BULK_SET && source != SOURCE_UART) {
        return 0; /* A preset, a GPIO or the firmware itself: no Link client */
    }

    /* Order, not recency: the oldest write still inside the window claims
     * the notification, so a second client's later write cannot take the
     * first client's attribution. */
    const uint32_t now = hub_now(hub);
    while (hub->origin_count > 0) {
        const hub_origin_t oldest = hub->origins[hub->origin_head];
        hub->origin_head = (hub->origin_head + 1) % HUB_ORIGIN_QUEUE;
        hub->origin_count--;
        if (now - oldest.sent_ms <= LINK_ORIGIN_WINDOW_MS) {
            return oldest.session_id;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Client commands
 * ------------------------------------------------------------------------- */

static void respond(hub_t *hub, int slot, uint16_t tag, uint8_t status, const uint8_t *payload,
                    uint16_t length)
{
    const size_t frame_length =
        link_encode_cmd_response(hub->frame, sizeof(hub->frame), tag, status, payload, length);
    hub_send_binary(hub, slot, hub->frame, frame_length);
}

/* The checks of bridge spec 8.3, in order. Returns the status to answer
 * with, or DSPI_STATUS_OK if the command may go to the DSPi. */
static uint8_t admission_status(const hub_t *hub, const hub_session_t *session,
                                const link_cmd_request_t *command, link_class_t class_)
{
    if (command->handle != HUB_DEVICE_HANDLE || !hub_device_online(hub)) {
        return LINK_STATUS_NO_DEVICE;
    }
    if (command->length > LINK_MAX_PAYLOAD_BYTES) {
        return LINK_STATUS_TOO_LARGE;
    }
    if (!link_policy_role_allows(session->role, class_)) {
        return LINK_STATUS_DENIED;
    }
    if (hub->device.locked_by != 0 && hub->device.locked_by != session->id) {
        return LINK_STATUS_LOCKED;
    }
    if (session->commands_in_flight >= LINK_MAX_INFLIGHT) {
        return LINK_STATUS_RATE_LIMITED;
    }
    return DSPI_STATUS_OK;
}

void hub_commands_handle_frame(hub_t *hub, int slot, const uint8_t *data, size_t length)
{
    hub_session_t *session = hub_open_session(hub, slot);
    link_cmd_request_t command;
    if (session == NULL) {
        return;
    }
    if (!link_decode_cmd_request(data, length, &command)) {
        respond(hub, slot, link_frame_tag(data, length), LINK_STATUS_BAD_FRAME, NULL, 0);
        return;
    }

    const link_class_t class_ = link_policy_class(command.request, command.is_get);
    const uint8_t status = admission_status(hub, session, &command, class_);
    if (status != DSPI_STATUS_OK) {
        respond(hub, slot, command.tag, status, NULL, 0);
        return;
    }

    /* Where the SET payload lives until the uart task has sent it. */
    const bool is_bulk_set = !command.is_get && command.request == REQ_SET_ALL_PARAMS;
    if (!command.is_get && !is_bulk_set && command.length > DSPI_UART_MAX_SET_PAYLOAD_BYTES) {
        /* The firmware refuses this on its UART (Link spec 9.2); answer as
         * it would, without spending the link on it. */
        respond(hub, slot, command.tag, DSPI_STATUS_OVERSIZE, NULL, 0);
        return;
    }
    if (is_bulk_set && hub->bulk_set_busy) {
        respond(hub, slot, command.tag, LINK_STATUS_RATE_LIMITED, NULL, 0);
        return;
    }

    hub_pending_t *entry = reserve_pending(hub);
    if (entry == NULL) {
        respond(hub, slot, command.tag, LINK_STATUS_RATE_LIMITED, NULL, 0);
        return;
    }
    entry->kind = HUB_WORK_CLIENT_COMMAND;
    entry->slot = slot;
    entry->session_id = session->id;
    entry->client_tag = command.tag;
    entry->is_set = !command.is_get;
    entry->is_bulk_set = is_bulk_set;

    uint8_t *payload_store = is_bulk_set ? hub->bulk_set_buffer : entry->payload;
    if (command.payload != NULL) {
        memcpy(payload_store, command.payload, command.length);
    }

    const dspi_request_t request = {
        .is_get = command.is_get,
        .request = command.request,
        .value = command.value,
        .index = command.index,
        .length = command.length,
        .payload = command.is_get ? NULL : payload_store,
    };
    /* Only a read is safe to repeat after no answer (bridge spec 7.3). */
    const bool may_retry = command.is_get && class_ == LINK_CLASS_READ;
    if (!hub->io.submit(hub->io.context, &request, may_retry, entry->tag)) {
        respond(hub, slot, command.tag, LINK_STATUS_RATE_LIMITED, NULL, 0);
        return;
    }

    commit_pending(hub);
    session->commands_in_flight++;
    hub->client_commands_pending++;
    if (is_bulk_set) {
        hub->bulk_set_busy = true;
    }
    if (entry->is_set) {
        remember_write(hub, session->id);
    }
}

/* The Link status for how a request ended (Link spec 8.1, bridge spec 7.3). */
static uint8_t link_status_for(const dspi_reply_t *reply)
{
    switch (reply->kind) {
    case DSPI_REPLY_ANSWERED:
        return reply->status; /* The DSPi's own status, unchanged */
    case DSPI_REPLY_TIMEOUT:
        return LINK_STATUS_TIMEOUT;
    case DSPI_REPLY_CORRUPT:
        return DSPI_STATUS_CRC_ERROR;
    case DSPI_REPLY_NO_DEVICE:
        return LINK_STATUS_NO_DEVICE;
    }
    return LINK_STATUS_TIMEOUT;
}

static void finish_client_command(hub_t *hub, const hub_pending_t *work, const dspi_reply_t *reply)
{
    const uint8_t status = link_status_for(reply);
    const uint32_t round_trip_ms = hub_now(hub) - work->submitted_ms;

    hub->device.commands++;
    hub->device.errors += status == DSPI_STATUS_OK ? 0 : 1;
    hub->device.average_rtt_ms = (hub->device.average_rtt_ms * 7 + round_trip_ms) / 8;
    if (work->is_set && status == DSPI_STATUS_OK) {
        hub_snapshot_mark_stale(hub);
    }

    /* The session may have closed, and its slot been reused, meanwhile. */
    hub_session_t *session = hub_open_session(hub, work->slot);
    if (session == NULL || session->id != work->session_id) {
        return;
    }
    session->commands_in_flight--;
    respond(hub, work->slot, work->client_tag, status, reply->payload,
            status == DSPI_STATUS_OK ? reply->length : 0);
}

void hub_reply(hub_t *hub, uint32_t tag, const dspi_reply_t *reply)
{
    /* Answers arrive in order. Anything older than `tag` will never be
     * answered (it cannot happen, but a lost entry must not jam the queue). */
    while (hub->pending_count > 0 && hub->pending[hub->pending_head].tag != tag) {
        hub_pending_t lost;
        pop_pending(hub, &lost);
    }
    if (hub->pending_count == 0) {
        return;
    }

    hub_pending_t work;
    pop_pending(hub, &work);

    switch (work.kind) {
    case HUB_WORK_CLIENT_COMMAND:
        hub->client_commands_pending--;
        if (work.is_bulk_set) {
            hub->bulk_set_busy = false; /* The bulk buffer is free again */
        }
        finish_client_command(hub, &work, reply);
        break;
    case HUB_WORK_POLL:
        hub->poll_pending = false;
        hub_polls_on_reply(hub, &work, reply);
        break;
    case HUB_WORK_SNAPSHOT_BULK:
    case HUB_WORK_SNAPSHOT_STATUS:
        hub_snapshot_on_reply(hub, &work, reply);
        break;
    }

    /* Keep the link busy: the next due poll can start now. */
    hub_polls_tick(hub);
}
