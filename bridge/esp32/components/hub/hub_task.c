/*
 * hub_task.c - the hub task and the ESP32 side of the hub's I/O. See
 * hub_task.h; implements bridge spec 6.2.
 *
 * Three kinds of code meet here:
 *
 *   producers  the HTTP server's handlers and the uart task's handlers,
 *              which only copy what they receive into an event and post it
 *   the loop   the hub task, which takes one event at a time and calls the
 *              hub, and calls hub_tick() every HUB_TICK_MS
 *   hub_io     the functions the hub calls back through, which reach the
 *              HTTP server, the uart task, flash and mDNS
 */

#include "hub_task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "dspi_uart.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_server.h"
#include "hub.h"
#include "net.h"
#include "settings.h"

static const char *TAG = "hub";

#define HUB_TASK_STACK_BYTES 6144
#define HUB_TASK_PRIORITY 5
#define HUB_EVENT_QUEUE_DEPTH 64
#define HUB_EVENT_POST_TIMEOUT_MS 1000

/* ---------------------------------------------------------------------------
 * Events
 * ------------------------------------------------------------------------- */

typedef enum {
    EVENT_SESSION_OPENED,
    EVENT_SESSION_MESSAGE,
    EVENT_SESSION_ACTIVITY,
    EVENT_SESSION_CLOSED,
    EVENT_DEVICE_FOUND,
    EVENT_DEVICE_LOST,
    EVENT_NOTIFICATION,
    EVENT_REPLY,
} event_type_t;

/* One event. `data` is a heap buffer the hub task frees after handling. */
typedef struct {
    event_type_t type;
    int slot;
    bool binary;
    uint8_t *data;
    size_t length;
    uint32_t tag;
    dspi_reply_t reply;        /* EVENT_REPLY; its payload is `data` */
    dspi_identity_t *identity; /* EVENT_DEVICE_FOUND, heap */
    uint32_t baud;
} hub_event_t;

static struct {
    hub_t hub;
    bridge_settings_t settings;
    QueueHandle_t events;

    /* The /dspi/v1/info document, rebuilt on the hub task whenever the hub
     * advertises and copied by the HTTP server's task. */
    char *info_json;
    SemaphoreHandle_t info_lock;
} task_state;

/* Copies `length` bytes onto the heap, or returns NULL for 0 bytes. */
static uint8_t *copy_bytes(const uint8_t *data, size_t length)
{
    if (length == 0) {
        return NULL;
    }
    uint8_t *copy = malloc(length);
    if (copy != NULL) {
        memcpy(copy, data, length);
    }
    return copy;
}

static void post(hub_event_t *event)
{
    if (xQueueSend(task_state.events, event, pdMS_TO_TICKS(HUB_EVENT_POST_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Hub queue full; dropped event %d", (int)event->type);
        free(event->data);
        free(event->identity);
    }
}

/* ---------------------------------------------------------------------------
 * Producers: the HTTP server's handlers (on its task)
 * ------------------------------------------------------------------------- */

static void on_session_opened(void *context, int slot)
{
    (void)context;
    post(&(hub_event_t){.type = EVENT_SESSION_OPENED, .slot = slot});
}

static void on_session_message(void *context, int slot, bool binary, uint8_t *data, size_t length)
{
    (void)context;
    post(&(hub_event_t){.type = EVENT_SESSION_MESSAGE,
                        .slot = slot,
                        .binary = binary,
                        .data = data,
                        .length = length});
}

static void on_session_activity(void *context, int slot)
{
    (void)context;
    post(&(hub_event_t){.type = EVENT_SESSION_ACTIVITY, .slot = slot});
}

static void on_session_closed(void *context, int slot)
{
    (void)context;
    post(&(hub_event_t){.type = EVENT_SESSION_CLOSED, .slot = slot});
}

static char *copy_info_json(void *context)
{
    (void)context;
    char *copy = NULL;
    xSemaphoreTake(task_state.info_lock, portMAX_DELAY);
    if (task_state.info_json != NULL) {
        copy = strdup(task_state.info_json);
    }
    xSemaphoreGive(task_state.info_lock);
    return copy;
}

/* ---------------------------------------------------------------------------
 * Producers: the uart task's handlers (on its task)
 * ------------------------------------------------------------------------- */

static void on_device_found(void *context, const dspi_identity_t *identity, uint32_t baud)
{
    (void)context;
    dspi_identity_t *copy = malloc(sizeof(*copy));
    if (copy == NULL) {
        return;
    }
    *copy = *identity;
    post(&(hub_event_t){.type = EVENT_DEVICE_FOUND, .identity = copy, .baud = baud});
}

static void on_device_lost(void *context)
{
    (void)context;
    post(&(hub_event_t){.type = EVENT_DEVICE_LOST});
}

static void on_notification(void *context, const uint8_t *packet, size_t length)
{
    (void)context;
    post(&(hub_event_t){
        .type = EVENT_NOTIFICATION, .data = copy_bytes(packet, length), .length = length});
}

static void on_reply(void *context, uint32_t tag, const dspi_reply_t *reply)
{
    (void)context;
    hub_event_t event = {.type = EVENT_REPLY, .tag = tag, .reply = *reply};
    event.data = copy_bytes(reply->payload, reply->length);
    event.length = reply->length;
    post(&event);
}

/* ---------------------------------------------------------------------------
 * hub_io: what the hub reaches (on the hub task)
 * ------------------------------------------------------------------------- */

static bool io_send_text(void *context, int slot, const char *text, size_t length)
{
    (void)context;
    const bool sent = http_server_send(slot, false, (const uint8_t *)text, length);
    if (!sent) {
        /* A JSON reply or event lost is worth knowing about; a dropped
         * NOTIFY or POLL frame is routine under load and is not logged. */
        ESP_LOGW(TAG, "Session %d: could not queue a %u-byte message", slot, (unsigned)length);
    }
    return sent;
}

static bool io_send_binary(void *context, int slot, const uint8_t *data, size_t length)
{
    (void)context;
    return http_server_send(slot, true, data, length);
}

static void io_ping(void *context, int slot)
{
    (void)context;
    http_server_ping(slot);
}

static void io_close(void *context, int slot, uint16_t code)
{
    (void)context;
    http_server_close(slot, code);
}

static bool io_submit(void *context, const dspi_request_t *request, bool may_retry, uint32_t tag)
{
    (void)context;
    return dspi_uart_submit(request, may_retry, tag) == ESP_OK;
}

static uint32_t io_now_ms(void *context)
{
    (void)context;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void io_save_hub_name(void *context, const char *name)
{
    (void)context;
    snprintf(task_state.settings.name, sizeof(task_state.settings.name), "%s", name);
    const esp_err_t err = settings_save(&task_state.settings);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save the hub name: %s", esp_err_to_name(err));
    }
}

static void io_save_device_name(void *context, const char *serial, const char *name)
{
    (void)context;
    const esp_err_t err = settings_save_device_name(serial, name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save the name of %s: %s", serial, esp_err_to_name(err));
    }
}

static bool io_load_device_name(void *context, const char *serial, char *name, size_t size)
{
    (void)context;
    return settings_load_device_name(serial, name, size) == ESP_OK;
}

/* Refreshes the DNS-SD record and the info document (Link spec 3.1, 3.2). */
static void io_advertise(void *context)
{
    (void)context;
    hub_advert_t advert;
    hub_advert(&task_state.hub, &advert);

    char devices[4];
    snprintf(devices, sizeof(devices), "%u", advert.devices);
    net_txt_entry_t txt[] = {
        {"v", "1"},     {"hid", advert.hub_id}, {"kind", "bridge"},   {"auth", advert.auth},
        {"n", devices}, {"web", "1"},           {"d", advert.serial},
    };
    /* `d` is last, so leaving it out with no device is a shorter count. */
    const size_t count = sizeof(txt) / sizeof(txt[0]) - (advert.serial == NULL ? 1 : 0);
    const esp_err_t err = net_advertise(advert.name, txt, count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not update the DNS-SD record: %s", esp_err_to_name(err));
    }

    char *info = hub_info_json(&task_state.hub);
    xSemaphoreTake(task_state.info_lock, portMAX_DELAY);
    cJSON_free(task_state.info_json);
    task_state.info_json = info;
    xSemaphoreGive(task_state.info_lock);
}

/* The `bridge` object of hub.stats (bridge spec 8.7). */
static void io_describe_bridge(void *context, cJSON *bridge)
{
    (void)context;
    cJSON_AddStringToObject(bridge, "chip", board_table()->chip_name);
    cJSON_AddBoolToObject(bridge, "psram", board_has_psram());
    cJSON_AddNumberToObject(bridge, "heap_free", heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    cJSON_AddNumberToObject(bridge, "uptime_s", (double)(esp_timer_get_time() / 1000000));

    net_status_t network;
    net_get_status(&network);
    cJSON *wifi = cJSON_AddObjectToObject(bridge, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", network.ssid);
    cJSON_AddNumberToObject(wifi, "rssi", network.rssi);
    cJSON_AddStringToObject(wifi, "ip", network.ip);
    cJSON_AddStringToObject(wifi, "mac", network.mac);

    dspi_uart_status_t link;
    dspi_uart_get_status(&link);
    cJSON *uart = cJSON_AddObjectToObject(bridge, "uart");
    cJSON_AddNumberToObject(uart, "baud", link.baud);
    cJSON_AddNumberToObject(uart, "tx_pin", link.tx_pin);
    cJSON_AddNumberToObject(uart, "rx_pin", link.rx_pin);
    cJSON_AddBoolToObject(uart, "live", link.device_present);
    cJSON_AddBoolToObject(uart, "notify_enable", task_state.hub.device.identity.notify_enabled);
    cJSON_AddNumberToObject(uart, "timeouts", link.counters.timeouts);
    cJSON_AddNumberToObject(uart, "crc_errors", link.counters.crc_errors);
    cJSON_AddNumberToObject(uart, "retries", link.counters.retries);
}

/* ---------------------------------------------------------------------------
 * The loop
 * ------------------------------------------------------------------------- */

static void handle_event(hub_event_t *event)
{
    hub_t *hub = &task_state.hub;
    switch (event->type) {
    case EVENT_SESSION_OPENED:
        hub_session_opened(hub, event->slot);
        break;
    case EVENT_SESSION_MESSAGE:
        if (event->binary) {
            hub_session_binary(hub, event->slot, event->data, event->length);
        } else {
            hub_session_text(hub, event->slot, (const char *)event->data, event->length);
        }
        break;
    case EVENT_SESSION_ACTIVITY:
        hub_session_activity(hub, event->slot);
        break;
    case EVENT_SESSION_CLOSED:
        hub_session_closed(hub, event->slot);
        http_server_release_slot(event->slot); /* Only now may the slot be reused */
        break;
    case EVENT_DEVICE_FOUND:
        hub_device_found(hub, event->identity, event->baud);
        if (event->baud != task_state.settings.uart_baud) {
            /* Detection found a new rate; start there next time (bridge
             * spec 7.5 and 8.11). */
            task_state.settings.uart_baud = event->baud;
            settings_save(&task_state.settings);
        }
        break;
    case EVENT_DEVICE_LOST:
        hub_device_lost(hub);
        break;
    case EVENT_NOTIFICATION:
        hub_notification(hub, event->data, event->length);
        break;
    case EVENT_REPLY:
        event->reply.payload = event->data;
        hub_reply(hub, event->tag, &event->reply);
        break;
    }
    free(event->data);
    free(event->identity);
}

static void hub_task(void *argument)
{
    (void)argument;
    int64_t next_tick_us = esp_timer_get_time();
    for (;;) {
        const int64_t wait_us = next_tick_us - esp_timer_get_time();
        const TickType_t wait = wait_us > 0 ? pdMS_TO_TICKS((uint32_t)(wait_us / 1000)) : 0;

        hub_event_t event;
        if (xQueueReceive(task_state.events, &event, wait) == pdTRUE) {
            handle_event(&event);
        }
        if (esp_timer_get_time() >= next_tick_us) {
            hub_tick(&task_state.hub);
            http_server_close_stalled();
            next_tick_us += (int64_t)HUB_TICK_MS * 1000;
        }
    }
}

esp_err_t hub_task_start(const bridge_settings_t *settings)
{
    task_state.settings = *settings;
    task_state.events = xQueueCreate(HUB_EVENT_QUEUE_DEPTH, sizeof(hub_event_t));
    task_state.info_lock = xSemaphoreCreateMutex();
    if (task_state.events == NULL || task_state.info_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    hub_config_t config = {
        .version = esp_app_get_description()->version,
        .max_sessions = board_max_sessions(),
    };
    snprintf(config.hub_id, sizeof(config.hub_id), "%s", settings->hub_id);
    snprintf(config.name, sizeof(config.name), "%s", settings->name);

    const hub_io_t io = {
        .send_text = io_send_text,
        .send_binary = io_send_binary,
        .ping = io_ping,
        .close = io_close,
        .submit = io_submit,
        .now_ms = io_now_ms,
        .save_hub_name = io_save_hub_name,
        .save_device_name = io_save_device_name,
        .load_device_name = io_load_device_name,
        .advertise = io_advertise,
        .describe_bridge = io_describe_bridge,
    };
    hub_init(&task_state.hub, &io, &config);
    io_advertise(NULL); /* The service exists from the start, with n=0 */

    if (xTaskCreate(hub_task, "hub", HUB_TASK_STACK_BYTES, NULL, HUB_TASK_PRIORITY, NULL) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    const dspi_uart_config_t uart = {
        .tx_pin = settings->uart_tx_pin,
        .rx_pin = settings->uart_rx_pin,
        .baud = settings->uart_baud,
        .detect_baud = settings->uart_detect_baud,
        .handlers = {.device_found = on_device_found,
                     .device_lost = on_device_lost,
                     .notification = on_notification,
                     .reply = on_reply},
    };
    ESP_RETURN_ON_ERROR(dspi_uart_start(&uart), TAG, "starting the uart task");

    const http_server_config_t http = {
        .max_sessions = config.max_sessions,
        .handlers = {.session_opened = on_session_opened,
                     .session_message = on_session_message,
                     .session_activity = on_session_activity,
                     .session_closed = on_session_closed,
                     .info_json = copy_info_json},
    };
    return http_server_start(&http);
}
