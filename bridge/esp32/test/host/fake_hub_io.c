/*
 * fake_hub_io.c - see fake_hub_io.h.
 */

#include "fake_hub_io.h"

#include <stdio.h>
#include <string.h>

static bool record(fake_hub_io_t *fake, int slot, bool binary, const void *data, size_t length)
{
    if (fake->queue_full[slot] || fake->message_count == FAKE_IO_MAX_MESSAGES ||
        length > FAKE_IO_MESSAGE_BYTES) {
        return false;
    }
    fake_message_t *message = &fake->messages[fake->message_count++];
    message->slot = slot;
    message->binary = binary;
    memcpy(message->data, data, length);
    message->length = length;
    return true;
}

static bool send_text(void *context, int slot, const char *text, size_t length)
{
    return record(context, slot, false, text, length);
}

static bool send_binary(void *context, int slot, const uint8_t *data, size_t length)
{
    return record(context, slot, true, data, length);
}

static void ping(void *context, int slot)
{
    fake_hub_io_t *fake = context;
    fake->pings[slot]++;
}

static void close_slot(void *context, int slot, uint16_t code)
{
    fake_hub_io_t *fake = context;
    fake->close_code[slot] = code;
}

static bool submit(void *context, const dspi_request_t *request, bool may_retry, uint32_t tag)
{
    fake_hub_io_t *fake = context;
    if (fake->submission_count == FAKE_IO_MAX_SUBMISSIONS) {
        return false;
    }
    fake_submission_t *entry = &fake->submissions[fake->submission_count++];
    entry->request = *request;
    if (!request->is_get && request->length > 0) {
        memcpy(entry->payload, request->payload, request->length);
        entry->request.payload = entry->payload;
    }
    entry->may_retry = may_retry;
    entry->tag = tag;
    return true;
}

static uint32_t now_ms(void *context)
{
    const fake_hub_io_t *fake = context;
    return fake->now_ms;
}

static void apply_settings(void *context, const bridge_settings_t *settings)
{
    fake_hub_io_t *fake = context;
    fake->applied = *settings;
    fake->apply_count++;
}

static void start_wifi_scan(void *context, int slot, uint16_t session_id, int request_id)
{
    fake_hub_io_t *fake = context;
    fake->scan_slot = slot;
    fake->scan_session = session_id;
    fake->scan_request = request_id;
}

static bool wifi_join(void *context, const char *ssid, const char *password)
{
    fake_hub_io_t *fake = context;
    (void)password;
    if (fake->refuse_join) {
        return false;
    }
    snprintf(fake->joined_ssid, sizeof(fake->joined_ssid), "%s", ssid);
    return true;
}

static void restart(void *context, hub_restart_level_t level)
{
    fake_hub_io_t *fake = context;
    fake->restart_level = (int)level;
}

static void save_device_name(void *context, const char *serial, const char *name)
{
    fake_hub_io_t *fake = context;
    snprintf(fake->saved_device_serial, sizeof(fake->saved_device_serial), "%s", serial);
    snprintf(fake->saved_device_name, sizeof(fake->saved_device_name), "%s", name);
}

static bool load_device_name(void *context, const char *serial, char *name, size_t size)
{
    const fake_hub_io_t *fake = context;
    if (strcmp(serial, fake->saved_device_serial) != 0 || fake->saved_device_name[0] == '\0') {
        return false;
    }
    snprintf(name, size, "%s", fake->saved_device_name);
    return true;
}

static void advertise(void *context)
{
    fake_hub_io_t *fake = context;
    fake->advertise_count++;
}

hub_io_t fake_hub_io_init(fake_hub_io_t *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->now_ms = 5000;
    fake->scan_slot = -1;
    fake->restart_level = -1;
    return (hub_io_t){
        .context = fake,
        .send_text = send_text,
        .send_binary = send_binary,
        .ping = ping,
        .close = close_slot,
        .submit = submit,
        .now_ms = now_ms,
        .apply_settings = apply_settings,
        .start_wifi_scan = start_wifi_scan,
        .wifi_join = wifi_join,
        .restart = restart,
        .save_device_name = save_device_name,
        .load_device_name = load_device_name,
        .advertise = advertise,
        .describe_bridge = NULL,
    };
}
