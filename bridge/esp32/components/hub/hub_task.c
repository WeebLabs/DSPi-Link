/*
 * hub_task.c - the hub task and the ESP32 side of the hub's I/O. See
 * hub_task.h; implements bridge spec 6.2.
 *
 * Three kinds of code meet here:
 *
 *   producers  the HTTP server's, uart task's and ota task's handlers,
 *              which only copy what they receive into an event and post it
 *   the loop   the hub task, which takes one event at a time and calls the
 *              hub, and calls hub_tick() every HUB_TICK_MS
 *   hub_io     the functions the hub calls back through, which reach the
 *              HTTP server, the uart task, flash, Wi-Fi, mDNS and updates
 *   calls      hub_task_*() functions other tasks use (the setup page),
 *              which run a function on the hub task and wait for it
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
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_server.h"
#include "hub.h"
#include "net.h"
#include "ota.h"
#include "psa/crypto.h"
#include "settings.h"
#include "status_led.h"
#include <time.h>

static const char *TAG = "hub";

#define HUB_TASK_STACK_BYTES 6144
#define HUB_TASK_PRIORITY 5
#define HUB_EVENT_QUEUE_DEPTH 64
#define HUB_EVENT_POST_TIMEOUT_MS 1000
#define HUB_CALL_TIMEOUT_MS 5000 /* Most a caller waits to queue its call */
#define SCAN_TASK_STACK_BYTES 4096
#define SCAN_MAX_NETWORKS 24

/* A restart waits this long so the replies and close frames go out first. */
#define RESTART_DELAY_MS 500

/* The hub task counts as alive if its loop has run this recently. It runs
 * every HUB_TICK_MS, so this allows for a long burst of work. */
#define HUB_ALIVE_WITHIN_MS 2000

/* Clock readings before this (2024-01-01) mean SNTP has not set the time. */
#define CLOCK_SET_AFTER 1704067200

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
    EVENT_REPLY_LATER,    /* A Wi-Fi scan's result, for hub_reply_later() */
    EVENT_CALL,           /* Run `call` on the hub task (hub_task_*() functions) */
    EVENT_UPDATE_WRITTEN, /* The ota task wrote a chunk */
    EVENT_UPDATE_DONE,    /* The ota task checked the whole image; `data` is its text */
} event_type_t;

/* A function to run on the hub task for another task, and its result. */
typedef struct {
    void (*run)(hub_t *hub, void *argument);
    void *argument;
    SemaphoreHandle_t done;
} hub_call_t;

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
    uint32_t address;    /* EVENT_SESSION_OPENED */
    uint16_t session_id; /* EVENT_REPLY_LATER */
    int request_id;      /* EVENT_REPLY_LATER */
    cJSON *body;         /* EVENT_REPLY_LATER, handed to the hub */
    hub_call_t *call;    /* EVENT_CALL */
    uint32_t offset;     /* EVENT_UPDATE_WRITTEN */
    bool ok;             /* EVENT_UPDATE_DONE */
} hub_event_t;

static struct {
    hub_t hub;
    bridge_settings_t settings;
    QueueHandle_t events;

    /* The /dspi/v1/info document and the hub's name, refreshed on the hub
     * task whenever the hub advertises, and copied by other tasks. */
    char *info_json;
    char name[SETTINGS_NAME_MAX_BYTES + 1];
    SemaphoreHandle_t info_lock;

    SemaphoreHandle_t scan_lock; /* One Wi-Fi scan at a time */
    esp_timer_handle_t restart_timer;
    hub_restart_level_t restart_level;
    int64_t last_loop_us; /* For hub_task_is_alive() */
} task_state;

/* The PIN and paired clients as saved: a layout number, then the store. A
 * firmware that changes hub_auth_store_t changes the size, and the old blob
 * is then ignored (settings_load_auth). */
typedef struct {
    uint32_t layout;
    hub_auth_store_t store;
} saved_auth_t;

#define SAVED_AUTH_LAYOUT 1

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

static void on_session_opened(void *context, int slot, uint32_t address)
{
    (void)context;
    post(&(hub_event_t){.type = EVENT_SESSION_OPENED, .slot = slot, .address = address});
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
 * Producers: the ota task's handlers (on its task)
 * ------------------------------------------------------------------------- */

static void on_update_written(void *context, uint32_t offset)
{
    (void)context;
    post(&(hub_event_t){.type = EVENT_UPDATE_WRITTEN, .offset = offset});
}

static void on_update_finished(void *context, bool ok, const char *text)
{
    (void)context;
    post(&(hub_event_t){.type = EVENT_UPDATE_DONE, .ok = ok, .data = (uint8_t *)strdup(text)});
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

/* Saves and applies new settings (bridge spec 8.11). Only what changed is
 * touched: the UART moves for new pins or rate, mDNS for a new hostname,
 * the status LED for a new type or pin.
 * The name reaches DNS-SD through the hub's advertise() that follows. */
static void io_apply_settings(void *context, const bridge_settings_t *settings)
{
    (void)context;
    const bridge_settings_t old = task_state.settings;
    task_state.settings = *settings;
    const esp_err_t err = settings_save(settings);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save the settings: %s", esp_err_to_name(err));
    }

    const bool link_changed =
        old.uart_tx_pin != settings->uart_tx_pin || old.uart_rx_pin != settings->uart_rx_pin ||
        old.uart_baud != settings->uart_baud || old.uart_detect_baud != settings->uart_detect_baud;
    if (link_changed) {
        dspi_uart_reconfigure(settings->uart_tx_pin, settings->uart_rx_pin, settings->uart_baud,
                              settings->uart_detect_baud);
    }
    if (strcmp(old.hostname, settings->hostname) != 0) {
        net_set_hostname(settings->hostname);
    }
    if (old.led_type != settings->led_type || old.led_pin != settings->led_pin) {
        status_led_configure(settings->led_type, settings->led_pin);
    }
}

/* Runs one scan off the hub task, which must not wait seconds for it, and
 * posts the answer back (Link spec 7.11). */
typedef struct {
    int slot;
    uint16_t session_id;
    int request_id;
} scan_request_t;

static void scan_task(void *argument)
{
    scan_request_t request = *(scan_request_t *)argument;
    free(argument);

    net_network_t *networks = calloc(SCAN_MAX_NETWORKS, sizeof(net_network_t));
    xSemaphoreTake(task_state.scan_lock, portMAX_DELAY);
    const size_t count = networks != NULL ? net_scan(networks, SCAN_MAX_NETWORKS) : 0;
    xSemaphoreGive(task_state.scan_lock);

    net_status_t network;
    net_get_status(&network);
    cJSON *body = cJSON_CreateObject();
    cJSON *current = cJSON_AddObjectToObject(body, "current");
    cJSON_AddStringToObject(current, "ssid", network.ssid);
    cJSON_AddNumberToObject(current, "rssi", network.rssi);
    cJSON_AddStringToObject(current, "ip", network.ip);
    cJSON *list = cJSON_AddArrayToObject(body, "networks");
    for (size_t i = 0; i < count; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "ssid", networks[i].ssid);
        cJSON_AddNumberToObject(entry, "rssi", networks[i].rssi);
        cJSON_AddBoolToObject(entry, "secure", networks[i].secure);
        cJSON_AddItemToArray(list, entry);
    }
    free(networks);

    hub_event_t event = {.type = EVENT_REPLY_LATER,
                         .slot = request.slot,
                         .session_id = request.session_id,
                         .request_id = request.request_id,
                         .body = body};
    if (xQueueSend(task_state.events, &event, pdMS_TO_TICKS(HUB_EVENT_POST_TIMEOUT_MS)) != pdTRUE) {
        cJSON_Delete(body);
    }
    vTaskDelete(NULL);
}

static void io_start_wifi_scan(void *context, int slot, uint16_t session_id, int request_id)
{
    (void)context;
    scan_request_t *request = malloc(sizeof(*request));
    if (request == NULL) {
        return;
    }
    *request = (scan_request_t){.slot = slot, .session_id = session_id, .request_id = request_id};
    if (xTaskCreate(scan_task, "wifi_scan", SCAN_TASK_STACK_BYTES, request, tskIDLE_PRIORITY + 2,
                    NULL) != pdPASS) {
        free(request);
    }
}

static bool io_wifi_join(void *context, const char *ssid, const char *password)
{
    (void)context;
    return net_join(ssid, password) == ESP_OK;
}

/* Erases what the level says, then restarts (Link spec 7.13). Runs from a
 * timer, so the replies and close frames have gone out first. */
static void restart_now(void *argument)
{
    (void)argument;
    switch (task_state.restart_level) {
    case HUB_RESTART_ONLY:
        break;
    case HUB_RESTART_NETWORK:
        settings_forget_wifi();
        break;
    case HUB_RESTART_FACTORY:
        settings_erase_all();
        break;
    }
    ESP_LOGW(TAG, "Restarting");
    esp_restart();
}

static void io_restart(void *context, hub_restart_level_t level)
{
    (void)context;
    task_state.restart_level = level;
    esp_timer_start_once(task_state.restart_timer, (uint64_t)RESTART_DELAY_MS * 1000);
}

/* ---------------------------------------------------------------------------
 * hub_auth_io: randomness, hashing, time and saving for the PIN
 * ------------------------------------------------------------------------- */

static void auth_random(void *context, uint8_t *out, size_t length)
{
    (void)context;
    esp_fill_random(out, length); /* A true random source once Wi-Fi runs */
}

static void auth_sha256(void *context, const uint8_t *data, size_t length, uint8_t out[32])
{
    (void)context;
    size_t written = 0;
    psa_hash_compute(PSA_ALG_SHA_256, data, length, out, 32, &written);
}

static void auth_pbkdf2(void *context, const char *pin, const uint8_t salt[AUTH_PIN_SALT_BYTES],
                        uint8_t out[32])
{
    (void)context;
    psa_key_derivation_operation_t operation = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_key_derivation_setup(&operation, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    psa_key_derivation_input_integer(&operation, PSA_KEY_DERIVATION_INPUT_COST,
                                     AUTH_PIN_PBKDF2_ITERATIONS);
    psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_SALT, salt,
                                   AUTH_PIN_SALT_BYTES);
    psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                   (const uint8_t *)pin, strlen(pin));
    if (psa_key_derivation_output_bytes(&operation, out, 32) != PSA_SUCCESS) {
        memset(out, 0, 32); /* Matches no PIN; logged once below */
        ESP_LOGE(TAG, "PBKDF2 failed");
    }
    psa_key_derivation_abort(&operation);
}

static uint32_t auth_unix_time(void *context)
{
    (void)context;
    const time_t now = time(NULL);
    return now > CLOCK_SET_AFTER ? (uint32_t)now : 0;
}

static void auth_save(void *context, const hub_auth_store_t *store)
{
    (void)context;
    const saved_auth_t saved = {.layout = SAVED_AUTH_LAYOUT, .store = *store};
    const esp_err_t err = settings_save_auth(&saved, sizeof(saved));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save the PIN data: %s", esp_err_to_name(err));
    }
}

static uint32_t auth_now_ms(void *context)
{
    (void)context;
    return (uint32_t)(esp_timer_get_time() / 1000);
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
    snprintf(task_state.name, sizeof(task_state.name), "%s", advert.name);
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

/* Firmware updates go straight to the ota task (bridge spec 14.2). */
static uint32_t io_update_capacity(void *context)
{
    (void)context;
    return ota_capacity();
}

static bool io_update_begin(void *context, uint32_t size, const uint8_t sha256[32])
{
    (void)context;
    return ota_begin(size, sha256);
}

static bool io_update_write(void *context, uint32_t offset, const uint8_t *data, size_t length)
{
    (void)context;
    return ota_write(offset, data, length);
}

static void io_update_abort(void *context)
{
    (void)context;
    ota_abort();
}

/* ---------------------------------------------------------------------------
 * The loop
 * ------------------------------------------------------------------------- */

static void handle_event(hub_event_t *event)
{
    hub_t *hub = &task_state.hub;
    switch (event->type) {
    case EVENT_SESSION_OPENED:
        hub_session_opened(hub, event->slot, event->address);
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
    case EVENT_REPLY_LATER:
        hub_reply_later(hub, event->slot, event->session_id, event->request_id, event->body);
        break;
    case EVENT_CALL:
        event->call->run(hub, event->call->argument);
        xSemaphoreGive(event->call->done);
        break;
    case EVENT_UPDATE_WRITTEN:
        hub_update_written(hub, event->offset);
        break;
    case EVENT_UPDATE_DONE:
        hub_update_finished(hub, event->ok,
                            event->data != NULL ? (const char *)event->data : "out of memory");
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

        task_state.last_loop_us = esp_timer_get_time();
        hub_event_t event;
        if (xQueueReceive(task_state.events, &event, wait) == pdTRUE) {
            handle_event(&event);
        }
        if (esp_timer_get_time() >= next_tick_us) {
            hub_tick(&task_state.hub);
            http_server_service();
            next_tick_us += (int64_t)HUB_TICK_MS * 1000;
        }
    }
}

esp_err_t hub_task_start(const bridge_settings_t *settings)
{
    task_state.settings = *settings;
    task_state.events = xQueueCreate(HUB_EVENT_QUEUE_DEPTH, sizeof(hub_event_t));
    task_state.info_lock = xSemaphoreCreateMutex();
    task_state.scan_lock = xSemaphoreCreateMutex();
    if (task_state.events == NULL || task_state.info_lock == NULL || task_state.scan_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t restart = {.callback = restart_now, .name = "restart"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&restart, &task_state.restart_timer), TAG,
                        "restart timer");
    ESP_RETURN_ON_FALSE(psa_crypto_init() == PSA_SUCCESS, ESP_FAIL, TAG, "starting PSA crypto");

    const hub_config_t config = {
        .settings = *settings,
        .board = board_table(),
        .version = esp_app_get_description()->version,
        .max_sessions = board_max_sessions(),
    };
    const hub_io_t io = {
        .send_text = io_send_text,
        .send_binary = io_send_binary,
        .ping = io_ping,
        .close = io_close,
        .submit = io_submit,
        .now_ms = io_now_ms,
        .apply_settings = io_apply_settings,
        .start_wifi_scan = io_start_wifi_scan,
        .wifi_join = io_wifi_join,
        .restart = io_restart,
        .save_device_name = io_save_device_name,
        .load_device_name = io_load_device_name,
        .advertise = io_advertise,
        .describe_bridge = io_describe_bridge,
        .update_capacity = io_update_capacity,
        .update_begin = io_update_begin,
        .update_write = io_update_write,
        .update_abort = io_update_abort,
    };
    const hub_auth_io_t auth_io = {
        .random = auth_random,
        .sha256 = auth_sha256,
        .pbkdf2 = auth_pbkdf2,
        .now_ms = auth_now_ms,
        .unix_time = auth_unix_time,
        .save = auth_save,
    };
    static saved_auth_t saved; /* Too big for app_main's stack */
    const bool have_saved =
        settings_load_auth(&saved, sizeof(saved)) == ESP_OK && saved.layout == SAVED_AUTH_LAYOUT;
    hub_init(&task_state.hub, &io, &config, &auth_io, have_saved ? &saved.store : NULL);
    ESP_LOGI(TAG, "Authentication: %s", hub_pin_enabled(&task_state.hub) ? "PIN" : "none");
    io_advertise(NULL); /* The service exists from the start, with n=0 */

    if (xTaskCreate(hub_task, "hub", HUB_TASK_STACK_BYTES, NULL, HUB_TASK_PRIORITY, NULL) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    const ota_handlers_t updates = {.written = on_update_written, .finished = on_update_finished};
    ESP_RETURN_ON_ERROR(ota_start(&updates), TAG, "starting the ota task");

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
    return dspi_uart_start(&uart);
}

http_server_handlers_t hub_task_http_handlers(void)
{
    return (http_server_handlers_t){
        .session_opened = on_session_opened,
        .session_message = on_session_message,
        .session_activity = on_session_activity,
        .session_closed = on_session_closed,
        .info_json = copy_info_json,
    };
}

/* ---------------------------------------------------------------------------
 * Calls from other tasks
 * ------------------------------------------------------------------------- */

/* Runs `run` on the hub task and waits for it to finish, so another task
 * can use the hub without sharing its state. */
static esp_err_t run_on_hub_task(void (*run)(hub_t *hub, void *argument), void *argument)
{
    hub_call_t call = {.run = run, .argument = argument, .done = xSemaphoreCreateBinary()};
    if (call.done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    hub_event_t event = {.type = EVENT_CALL, .call = &call};
    if (xQueueSend(task_state.events, &event, pdMS_TO_TICKS(HUB_CALL_TIMEOUT_MS)) != pdTRUE) {
        vSemaphoreDelete(call.done);
        return ESP_ERR_TIMEOUT;
    }
    /* Once queued, the hub task always runs the call, and `call` lives on
     * this stack, so wait for it however long it takes. */
    xSemaphoreTake(call.done, portMAX_DELAY);
    vSemaphoreDelete(call.done);
    return ESP_OK;
}

typedef struct {
    const char *name;
    bool ok;
} rename_call_t;

static void rename_on_hub(hub_t *hub, void *argument)
{
    rename_call_t *call = argument;
    call->ok = hub_rename(hub, call->name);
}

esp_err_t hub_task_rename(const char *name)
{
    rename_call_t call = {.name = name};
    ESP_RETURN_ON_ERROR(run_on_hub_task(rename_on_hub, &call), TAG, "renaming");
    return call.ok ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static void pin_enabled_on_hub(hub_t *hub, void *argument)
{
    *(bool *)argument = hub_pin_enabled(hub);
}

bool hub_task_pin_enabled(void)
{
    bool enabled = false;
    run_on_hub_task(pin_enabled_on_hub, &enabled);
    return enabled;
}

typedef struct {
    uint32_t address;
    const char *pin;
    hub_auth_result_t result;
} check_pin_call_t;

static void check_pin_on_hub(hub_t *hub, void *argument)
{
    check_pin_call_t *call = argument;
    call->result = hub_check_pin(hub, call->address, call->pin);
}

hub_auth_result_t hub_task_check_pin(uint32_t address, const char *pin)
{
    check_pin_call_t call = {.address = address, .pin = pin, .result = HUB_AUTH_WRONG};
    run_on_hub_task(check_pin_on_hub, &call);
    return call.result;
}

typedef struct {
    const char *pin;
    bool ok;
} set_pin_call_t;

static void set_pin_on_hub(hub_t *hub, void *argument)
{
    set_pin_call_t *call = argument;
    call->ok = hub_set_pin(hub, call->pin);
}

esp_err_t hub_task_set_pin(const char *pin)
{
    set_pin_call_t call = {.pin = pin};
    ESP_RETURN_ON_ERROR(run_on_hub_task(set_pin_on_hub, &call), TAG, "setting the PIN");
    return call.ok ? ESP_OK : ESP_ERR_INVALID_ARG;
}

bool hub_task_is_alive(void)
{
    return esp_timer_get_time() - task_state.last_loop_us < (int64_t)HUB_ALIVE_WITHIN_MS * 1000;
}

static void restart_on_hub(hub_t *hub, void *argument)
{
    hub_restart(hub, *(const hub_restart_level_t *)argument);
}

esp_err_t hub_task_restart(hub_restart_level_t level)
{
    return run_on_hub_task(restart_on_hub, &level);
}

void hub_task_name(char *name, size_t size)
{
    xSemaphoreTake(task_state.info_lock, portMAX_DELAY);
    snprintf(name, size, "%s", task_state.name);
    xSemaphoreGive(task_state.info_lock);
}
