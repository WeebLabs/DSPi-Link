/*
 * hub_task.c - the hub task and the ESP32 side of the hub's I/O. See
 * hub_task.h; implements bridge spec 6.2.
 *
 * Four kinds of code meet here:
 *
 *   producers  the HTTP server's, uart task's and ota task's handlers,
 *              which only copy what they receive into an event and post it
 *   the loop   the hub task, which takes one event at a time and calls the
 *              hub, and calls hub_tick() every HUB_TICK_MS
 *   hub_io     the functions the hub calls back through, which reach the
 *              HTTP server, the uart task, flash, Wi-Fi, mDNS and updates
 *   calls      hub_task_*() functions other tasks use (the setup page, the
 *              BOOT button), which post to the hub task
 *
 * Two rules keep events flowing. Answers and notifications travel in
 * buffers from a fixed pool, so the command and notification paths never
 * allocate. And the queue's last HUB_EVENT_LIFECYCLE_RESERVE places are
 * kept for events that must not be lost (a session opening or closing, the
 * device coming or going, update progress, a restart): ordinary events
 * never take them.
 */

#include "hub_task.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static const char *TAG = "hub";

_Static_assert(HUB_IO_SHA256_BYTES == OTA_SHA256_BYTES, "hub.update hands its digest to ota");

/* ---------------------------------------------------------------------------
 * Events
 * ------------------------------------------------------------------------- */

typedef enum {
    EVENT_SESSION_OPENED, /* Lifecycle */
    EVENT_SESSION_MESSAGE,
    EVENT_SESSION_ACTIVITY,
    EVENT_SESSION_CLOSED, /* Lifecycle */
    EVENT_DEVICE_FOUND,   /* Lifecycle; the identity is in task_state.found_identity */
    EVENT_DEVICE_LOST,    /* Lifecycle */
    EVENT_NOTIFICATION,
    EVENT_REPLY,
    EVENT_REPLY_LATER,    /* A Wi-Fi scan's result, for hub_reply_later() */
    EVENT_CALL,           /* Run `call` on the hub task (hub_task_*() functions) */
    EVENT_UPDATE_WRITTEN, /* Lifecycle: the ota task wrote a chunk */
    EVENT_UPDATE_DONE,    /* Lifecycle: the ota task checked the image; `data` is its text */
    EVENT_RESTART,        /* Lifecycle: hub_task_restart(), for the BOOT button */
} event_type_t;

/* A function to run on the hub task for another task, and its result. */
typedef struct {
    void (*run)(hub_t *hub, void *argument);
    void *argument;
    SemaphoreHandle_t done;
} hub_call_t;

/* One event. Whatever it carries is released by release_event() once it
 * has been handled or dropped. */
typedef struct {
    event_type_t type;
    int slot;
    bool binary;
    uint8_t *data;   /* Heap: a session's message, or an update's text */
    uint8_t *buffer; /* From the pool: an answer's or a notification's bytes */
    size_t length;
    uint32_t tag;
    dspi_reply_t reply; /* EVENT_REPLY; its payload is `buffer` */
    uint32_t baud;
    uint32_t address;          /* EVENT_SESSION_OPENED */
    uint16_t session_id;       /* EVENT_REPLY_LATER */
    int request_id;            /* EVENT_REPLY_LATER */
    cJSON *body;               /* EVENT_REPLY_LATER, handed to the hub */
    hub_call_t *call;          /* EVENT_CALL */
    uint32_t offset;           /* EVENT_UPDATE_WRITTEN */
    bool ok;                   /* EVENT_UPDATE_DONE */
    hub_restart_level_t level; /* EVENT_RESTART */
} hub_event_t;

static struct {
    hub_t hub;
    bridge_settings_t settings; /* What is in effect, for working out what changed */
    QueueHandle_t events;

    /* The event buffer pool: queues of free buffer pointers. */
    QueueHandle_t free_small;
    QueueHandle_t free_large;

    /* The latest identity the uart task reported, for EVENT_DEVICE_FOUND;
     * kept here rather than allocated per event. */
    dspi_identity_t found_identity;
    portMUX_TYPE found_identity_lock;

    /* The /dspi/v1/info document and the hub's name, refreshed on the hub
     * task whenever the hub advertises, and copied by other tasks. */
    char *info_json;
    char name[SETTINGS_NAME_MAX_BYTES + 1];
    SemaphoreHandle_t info_lock;

    atomic_bool scan_running; /* One Wi-Fi scan at a time */
    esp_timer_handle_t restart_timer;
    _Atomic hub_restart_level_t restart_level; /* Written by the hub task, read by the timer */
    _Atomic uint32_t last_loop_ms;             /* For hub_task_is_alive(), from any task */
} task_state = {.found_identity_lock = portMUX_INITIALIZER_UNLOCKED};

static uint8_t small_buffers[HUB_EVENT_SMALL_BUFFERS][HUB_EVENT_SMALL_BUFFER_BYTES];
static uint8_t large_buffers[HUB_EVENT_LARGE_BUFFERS][HUB_EVENT_LARGE_BUFFER_BYTES];

/* The PIN and paired clients as saved: a layout number, then the store. A
 * firmware that changes hub_auth_store_t changes the size, and the old blob
 * is then ignored (settings_load_auth). */
typedef struct {
    uint32_t layout;
    hub_auth_store_t store;
} saved_auth_t;

#define SAVED_AUTH_LAYOUT 1

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---------------------------------------------------------------------------
 * The event buffer pool
 * ------------------------------------------------------------------------- */

/* Fills the pool's free lists. Called once, before any producer runs. */
static esp_err_t pool_start(void)
{
    task_state.free_small = xQueueCreate(HUB_EVENT_SMALL_BUFFERS, sizeof(uint8_t *));
    task_state.free_large = xQueueCreate(HUB_EVENT_LARGE_BUFFERS, sizeof(uint8_t *));
    if (task_state.free_small == NULL || task_state.free_large == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* The queues are exactly as deep as the pools, so these sends fit. */
    for (size_t i = 0; i < HUB_EVENT_SMALL_BUFFERS; i++) {
        uint8_t *buffer = small_buffers[i];
        (void)xQueueSend(task_state.free_small, &buffer, 0);
    }
    for (size_t i = 0; i < HUB_EVENT_LARGE_BUFFERS; i++) {
        uint8_t *buffer = large_buffers[i];
        (void)xQueueSend(task_state.free_large, &buffer, 0);
    }
    return ESP_OK;
}

/* A free buffer holding `length` bytes, or NULL if none is free. A short
 * message takes a large buffer only when the small ones have run out. */
static uint8_t *pool_take(size_t length)
{
    uint8_t *buffer = NULL;
    if (length <= HUB_EVENT_SMALL_BUFFER_BYTES &&
        xQueueReceive(task_state.free_small, &buffer, 0) == pdTRUE) {
        return buffer;
    }
    if (length <= HUB_EVENT_LARGE_BUFFER_BYTES &&
        xQueueReceive(task_state.free_large, &buffer, 0) == pdTRUE) {
        return buffer;
    }
    return NULL;
}

static void pool_give(uint8_t *buffer)
{
    if (buffer == NULL) {
        return;
    }
    const uintptr_t address = (uintptr_t)buffer;
    const uintptr_t large_start = (uintptr_t)large_buffers;
    const bool large = address >= large_start && address < large_start + sizeof(large_buffers);
    /* Each list has room for every buffer of its pool, so this never waits. */
    if (xQueueSend(large ? task_state.free_large : task_state.free_small, &buffer, 0) != pdTRUE) {
        ESP_LOGE(TAG, "An event buffer was returned twice");
    }
}

/* ---------------------------------------------------------------------------
 * Posting events
 * ------------------------------------------------------------------------- */

static void release_event(hub_event_t *event)
{
    free(event->data);
    pool_give(event->buffer);
    cJSON_Delete(event->body);
}

/* Posts an ordinary event, waiting up to `timeout_ms` for a place outside
 * the lifecycle reserve. The space check and the send are not one step, so
 * tasks racing for the last ordinary place can take a few reserved ones;
 * the reserve counts one place per posting task for that. */
static bool post_ordinary(const hub_event_t *event, uint32_t timeout_ms)
{
    const uint32_t started_ms = now_ms();
    for (;;) {
        if (uxQueueSpacesAvailable(task_state.events) > HUB_EVENT_LIFECYCLE_RESERVE &&
            xQueueSend(task_state.events, event, 0) == pdTRUE) {
            return true;
        }
        if (now_ms() - started_ms >= timeout_ms) {
            return false;
        }
        vTaskDelay(1);
    }
}

/* Posts an ordinary event, or drops it, releasing what it carries. */
static void post(hub_event_t *event)
{
    if (!post_ordinary(event, HUB_EVENT_POST_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "Hub queue full; dropped event %d", (int)event->type);
        release_event(event);
    }
}

/* Posts a lifecycle event, which may use the reserve. */
static void post_lifecycle(hub_event_t *event)
{
    if (xQueueSend(task_state.events, event, pdMS_TO_TICKS(HUB_LIFECYCLE_POST_TIMEOUT_MS)) !=
        pdTRUE) {
        ESP_LOGE(TAG, "Hub queue full even for event %d; dropped", (int)event->type);
        release_event(event);
    }
}

/* ---------------------------------------------------------------------------
 * Producers: the HTTP server's handlers (on its task)
 * ------------------------------------------------------------------------- */

static void on_session_opened(void *context, int slot, uint32_t address)
{
    (void)context;
    post_lifecycle(&(hub_event_t){.type = EVENT_SESSION_OPENED, .slot = slot, .address = address});
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
    post_lifecycle(&(hub_event_t){.type = EVENT_SESSION_CLOSED, .slot = slot});
}

static char *copy_info_json(void *context)
{
    (void)context;
    if (xSemaphoreTake(task_state.info_lock, pdMS_TO_TICKS(HUB_INFO_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Info document locked too long");
        return NULL;
    }
    char *copy = task_state.info_json != NULL ? strdup(task_state.info_json) : NULL;
    xSemaphoreGive(task_state.info_lock);
    return copy; /* NULL makes the server answer with an error */
}

/* ---------------------------------------------------------------------------
 * Producers: the ota task's handlers (on its task)
 * ------------------------------------------------------------------------- */

static void on_update_written(void *context, uint32_t offset)
{
    (void)context;
    post_lifecycle(&(hub_event_t){.type = EVENT_UPDATE_WRITTEN, .offset = offset});
}

static void on_update_finished(void *context, bool ok, const char *text)
{
    (void)context;
    /* NULL when memory ran out; the loop then says so instead. */
    post_lifecycle(
        &(hub_event_t){.type = EVENT_UPDATE_DONE, .ok = ok, .data = (uint8_t *)strdup(text)});
}

/* ---------------------------------------------------------------------------
 * Producers: the uart task's handlers (on its task)
 * ------------------------------------------------------------------------- */

static void on_device_found(void *context, const dspi_identity_t *identity, uint32_t baud)
{
    (void)context;
    taskENTER_CRITICAL(&task_state.found_identity_lock);
    task_state.found_identity = *identity;
    taskEXIT_CRITICAL(&task_state.found_identity_lock);
    post_lifecycle(&(hub_event_t){.type = EVENT_DEVICE_FOUND, .baud = baud});
}

static void on_device_lost(void *context)
{
    (void)context;
    post_lifecycle(&(hub_event_t){.type = EVENT_DEVICE_LOST});
}

static void on_notification(void *context, const uint8_t *packet, size_t length)
{
    (void)context;
    uint8_t *buffer = pool_take(length);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "No buffer for a %u-byte notification; dropped", (unsigned)length);
        return; /* The seq gap makes every client re-read (bridge spec 8.4) */
    }
    memcpy(buffer, packet, length);
    post(&(hub_event_t){.type = EVENT_NOTIFICATION, .buffer = buffer, .length = length});
}

static void on_reply(void *context, uint32_t tag, const dspi_reply_t *reply)
{
    (void)context;
    hub_event_t event = {.type = EVENT_REPLY, .tag = tag, .reply = *reply};
    event.reply.payload = NULL;
    if (reply->length > 0) {
        event.buffer = pool_take(reply->length);
        if (event.buffer != NULL) {
            memcpy(event.buffer, reply->payload, reply->length);
        } else {
            /* Still delivered, so the request finishes and its client hears
             * back, but as a timeout with nothing in it. */
            ESP_LOGE(TAG, "No buffer for a %u-byte answer; reported as a timeout",
                     (unsigned)reply->length);
            event.reply = (dspi_reply_t){.kind = DSPI_REPLY_TIMEOUT};
        }
    }
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
    return now_ms();
}

/* Saves and applies new settings (bridge spec 8.11). Only what changed is
 * touched: the UART moves for new pins or rate, mDNS for a new hostname,
 * the status LED for a new type or pin. The name reaches DNS-SD through
 * the hub's advertise() that follows. A part that fails stays as it was in
 * task_state.settings, so asking again retries it. */
static bool io_apply_settings(void *context, const bridge_settings_t *settings)
{
    (void)context;
    esp_err_t err = settings_save(settings);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save the settings: %s", esp_err_to_name(err));
        return false;
    }

    const bridge_settings_t old = task_state.settings;
    bridge_settings_t running = *settings;
    bool applied = true;
    if (old.uart_tx_pin != settings->uart_tx_pin || old.uart_rx_pin != settings->uart_rx_pin ||
        old.uart_baud != settings->uart_baud ||
        old.uart_detect_baud != settings->uart_detect_baud) {
        err = dspi_uart_reconfigure(settings->uart_tx_pin, settings->uart_rx_pin,
                                    settings->uart_baud, settings->uart_detect_baud);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not move the UART (%s); saved for the next start",
                     esp_err_to_name(err));
            running.uart_tx_pin = old.uart_tx_pin;
            running.uart_rx_pin = old.uart_rx_pin;
            running.uart_baud = old.uart_baud;
            running.uart_detect_baud = old.uart_detect_baud;
            applied = false;
        }
    }
    if (strcmp(old.hostname, settings->hostname) != 0) {
        err = net_set_hostname(settings->hostname);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not change the hostname: %s", esp_err_to_name(err));
            memcpy(running.hostname, old.hostname, sizeof(running.hostname));
            applied = false;
        }
    }
    if (old.led_type != settings->led_type || old.led_pin != settings->led_pin) {
        err = status_led_configure(settings->led_type, settings->led_pin);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not change the status LED: %s", esp_err_to_name(err));
            running.led_type = old.led_type;
            running.led_pin = old.led_pin;
            applied = false;
        }
    }
    task_state.settings = running;
    return applied;
}

/* Runs one scan off the hub task, which must not wait seconds for it, and
 * posts the answer back (Link spec 7.11). */
typedef struct {
    int slot;
    uint16_t session_id;
    int request_id;
} scan_request_t;

/* The hub.wifi.scan answer: the current network and `count` found. */
static cJSON *scan_body(const net_network_t *networks, size_t count)
{
    net_status_t network;
    net_get_status(&network);
    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        return NULL;
    }
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
        if (!cJSON_AddItemToArray(list, entry)) {
            cJSON_Delete(entry);
        }
    }
    return body;
}

static void scan_task(void *argument)
{
    const scan_request_t request = *(scan_request_t *)argument;
    free(argument);

    net_network_t *networks = calloc(WIFI_SCAN_MAX_NETWORKS, sizeof(net_network_t));
    if (networks == NULL) {
        ESP_LOGE(TAG, "No memory for a Wi-Fi scan; answering with no networks");
    }
    const size_t count = networks != NULL ? net_scan(networks, WIFI_SCAN_MAX_NETWORKS) : 0;
    cJSON *body = scan_body(networks, count);
    free(networks);
    atomic_store(&task_state.scan_running, false);

    hub_event_t event = {.type = EVENT_REPLY_LATER,
                         .slot = request.slot,
                         .session_id = request.session_id,
                         .request_id = request.request_id,
                         .body = body};
    if (body == NULL) {
        ESP_LOGE(TAG, "No memory for the Wi-Fi scan's answer");
    } else {
        post(&event);
    }
    vTaskDelete(NULL);
}

static hub_io_result_t io_start_wifi_scan(void *context, int slot, uint16_t session_id,
                                          int request_id)
{
    (void)context;
    if (atomic_exchange(&task_state.scan_running, true)) {
        return HUB_IO_BUSY;
    }
    scan_request_t *request = malloc(sizeof(*request));
    if (request != NULL) {
        *request =
            (scan_request_t){.slot = slot, .session_id = session_id, .request_id = request_id};
        if (xTaskCreate(scan_task, "wifi_scan", WIFI_SCAN_TASK_STACK_BYTES, request,
                        WIFI_SCAN_TASK_PRIORITY, NULL) == pdPASS) {
            return HUB_IO_OK;
        }
    }
    ESP_LOGE(TAG, "Could not start a Wi-Fi scan task");
    free(request);
    atomic_store(&task_state.scan_running, false);
    return HUB_IO_FAILED;
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
    esp_err_t err = ESP_OK;
    switch (atomic_load(&task_state.restart_level)) {
    case HUB_RESTART_ONLY:
        break;
    case HUB_RESTART_NETWORK:
        err = settings_forget_wifi();
        break;
    case HUB_RESTART_FACTORY:
        err = settings_erase_all();
        break;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not erase before restarting: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "Restarting");
    esp_restart();
}

static void io_restart(void *context, hub_restart_level_t level)
{
    (void)context;
    /* Only the hub task writes the level, and only ever raises it, so a
     * restart asked for while a factory reset waits still erases. */
    if (level > atomic_load(&task_state.restart_level)) {
        atomic_store(&task_state.restart_level, level);
    }
    const esp_err_t err =
        esp_timer_start_once(task_state.restart_timer, (uint64_t)HUB_RESTART_DELAY_MS * 1000);
    if (err == ESP_ERR_INVALID_STATE) {
        return; /* Already on its way, now at the raised level */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not schedule the restart (%s); restarting now", esp_err_to_name(err));
        restart_now(NULL);
    }
}

/* ---------------------------------------------------------------------------
 * hub_auth_io: randomness, hashing, time and saving for the PIN
 * ------------------------------------------------------------------------- */

static bool auth_random(void *context, uint8_t *out, size_t length)
{
    (void)context;
    esp_fill_random(out, length); /* A true random source once Wi-Fi runs; cannot fail */
    return true;
}

static bool auth_sha256(void *context, const uint8_t *data, size_t length,
                        uint8_t out[HUB_AUTH_HASH_BYTES])
{
    (void)context;
    size_t written = 0;
    const psa_status_t status =
        psa_hash_compute(PSA_ALG_SHA_256, data, length, out, HUB_AUTH_HASH_BYTES, &written);
    if (status != PSA_SUCCESS || written != HUB_AUTH_HASH_BYTES) {
        ESP_LOGE(TAG, "SHA-256 failed: %d", (int)status);
        return false;
    }
    return true;
}

static bool auth_pbkdf2(void *context, const char *pin, const uint8_t salt[AUTH_PIN_SALT_BYTES],
                        uint8_t out[HUB_AUTH_HASH_BYTES])
{
    (void)context;
    psa_key_derivation_operation_t operation = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t status =
        psa_key_derivation_setup(&operation, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_integer(&operation, PSA_KEY_DERIVATION_INPUT_COST,
                                                  AUTH_PIN_PBKDF2_ITERATIONS);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_SALT, salt,
                                                AUTH_PIN_SALT_BYTES);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                                (const uint8_t *)pin, strlen(pin));
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_output_bytes(&operation, out, HUB_AUTH_HASH_BYTES);
    }
    /* Only frees the operation; whether the hash was made is `status`. */
    (void)psa_key_derivation_abort(&operation);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "PBKDF2 failed: %d", (int)status);
        return false;
    }
    return true;
}

static uint32_t auth_unix_time(void *context)
{
    (void)context;
    const time_t now = time(NULL);
    return now > CLOCK_SET_AFTER_UNIX_S ? (uint32_t)now : 0;
}

static void auth_save(void *context, const hub_auth_store_t *store)
{
    (void)context;
    static saved_auth_t saved; /* Kept off the hub task's stack */
    saved = (saved_auth_t){.layout = SAVED_AUTH_LAYOUT, .store = *store};
    const esp_err_t err = settings_save_auth(&saved, sizeof(saved));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save the PIN data: %s", esp_err_to_name(err));
    }
}

static uint32_t auth_now_ms(void *context)
{
    (void)context;
    return now_ms();
}

static bool io_save_device_name(void *context, const char *serial, const char *name)
{
    (void)context;
    const esp_err_t err = settings_save_device_name(serial, name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save the name of %s: %s", serial, esp_err_to_name(err));
        return false;
    }
    return true;
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
        ESP_LOGE(TAG, "Could not update the DNS-SD record: %s", esp_err_to_name(err));
    }

    char *info = hub_info_json(&task_state.hub);
    if (info == NULL) {
        ESP_LOGE(TAG, "No memory for the info document; keeping the last one");
    }
    if (xSemaphoreTake(task_state.info_lock, pdMS_TO_TICKS(HUB_INFO_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Info document locked too long; not refreshed");
        cJSON_free(info);
        return;
    }
    if (info != NULL) {
        cJSON_free(task_state.info_json);
        task_state.info_json = info;
    }
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
    cJSON_AddNumberToObject(uart, "requests", link.counters.requests);
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

static bool io_update_begin(void *context, uint32_t size, const uint8_t sha256[HUB_IO_SHA256_BYTES])
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

static void handle_device_found(hub_t *hub, uint32_t baud)
{
    dspi_identity_t identity;
    taskENTER_CRITICAL(&task_state.found_identity_lock);
    identity = task_state.found_identity;
    taskEXIT_CRITICAL(&task_state.found_identity_lock);
    hub_device_found(hub, &identity, baud);
}

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
        handle_device_found(hub, event->baud);
        break;
    case EVENT_DEVICE_LOST:
        hub_device_lost(hub);
        break;
    case EVENT_NOTIFICATION:
        hub_notification(hub, event->buffer, event->length);
        break;
    case EVENT_REPLY:
        event->reply.payload = event->buffer;
        hub_reply(hub, event->tag, &event->reply);
        break;
    case EVENT_REPLY_LATER:
        hub_reply_later(hub, event->slot, event->session_id, event->request_id, event->body);
        event->body = NULL; /* The hub owns it now */
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
    case EVENT_RESTART:
        hub_restart(hub, event->level);
        break;
    }
    release_event(event);
}

static void hub_task(void *argument)
{
    (void)argument;
    int64_t next_tick_us = esp_timer_get_time();
    for (;;) {
        const int64_t wait_us = next_tick_us - esp_timer_get_time();
        const TickType_t wait = wait_us > 0 ? pdMS_TO_TICKS((uint32_t)(wait_us / 1000)) : 0;

        atomic_store(&task_state.last_loop_ms, now_ms());
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

static hub_io_t hub_io_functions(void)
{
    return (hub_io_t){
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
}

/* Starts the hub on `settings`, with the PIN and paired clients saved
 * before, if any. */
static void start_hub(const bridge_settings_t *settings)
{
    const hub_config_t config = {
        .settings = *settings,
        .board = board_table(),
        .version = esp_app_get_description()->version,
        .max_sessions = board_max_sessions(),
    };
    const hub_io_t io = hub_io_functions();
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
}

esp_err_t hub_task_start(const bridge_settings_t *settings)
{
    task_state.settings = *settings;
    task_state.events = xQueueCreate(HUB_EVENT_QUEUE_DEPTH, sizeof(hub_event_t));
    task_state.info_lock = xSemaphoreCreateMutex();
    if (task_state.events == NULL || task_state.info_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(pool_start(), TAG, "event buffers");
    const esp_timer_create_args_t restart = {.callback = restart_now, .name = "restart"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&restart, &task_state.restart_timer), TAG,
                        "restart timer");
    ESP_RETURN_ON_FALSE(psa_crypto_init() == PSA_SUCCESS, ESP_FAIL, TAG, "starting PSA crypto");

    start_hub(settings);
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
 * can use the hub without sharing its state. ESP_ERR_TIMEOUT if the hub
 * task's queue stayed full for HUB_CALL_TIMEOUT_MS. */
static esp_err_t run_on_hub_task(void (*run)(hub_t *hub, void *argument), void *argument)
{
    if (task_state.events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    hub_call_t call = {.run = run, .argument = argument, .done = xSemaphoreCreateBinary()};
    if (call.done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const hub_event_t event = {.type = EVENT_CALL, .call = &call};
    if (!post_ordinary(&event, HUB_CALL_TIMEOUT_MS)) {
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
    hub_change_result_t result;
} rename_call_t;

static void rename_on_hub(hub_t *hub, void *argument)
{
    rename_call_t *call = argument;
    call->result = hub_rename(hub, call->name);
}

esp_err_t hub_task_rename(const char *name)
{
    rename_call_t call = {.name = name, .result = HUB_CHANGE_FAILED};
    ESP_RETURN_ON_ERROR(run_on_hub_task(rename_on_hub, &call), TAG, "renaming");
    switch (call.result) {
    case HUB_CHANGE_OK:
        return ESP_OK;
    case HUB_CHANGE_INVALID:
        return ESP_ERR_INVALID_ARG;
    case HUB_CHANGE_FAILED:
        break;
    }
    return ESP_FAIL;
}

static void pin_enabled_on_hub(hub_t *hub, void *argument)
{
    *(bool *)argument = hub_pin_enabled(hub);
}

bool hub_task_pin_enabled(void)
{
    bool enabled = true;
    const esp_err_t err = run_on_hub_task(pin_enabled_on_hub, &enabled);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not ask whether the PIN is on (%s); assuming it is",
                 esp_err_to_name(err));
        return true; /* Fails closed: the caller asks for the PIN */
    }
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
    check_pin_call_t call = {.address = address, .pin = pin, .result = HUB_AUTH_UNAVAILABLE};
    const esp_err_t err = run_on_hub_task(check_pin_on_hub, &call);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not check a PIN: %s", esp_err_to_name(err));
        return HUB_AUTH_UNAVAILABLE; /* Fails closed */
    }
    return call.result;
}

typedef struct {
    const char *pin;
    hub_auth_result_t result;
} set_pin_call_t;

static void set_pin_on_hub(hub_t *hub, void *argument)
{
    set_pin_call_t *call = argument;
    call->result = hub_set_pin(hub, call->pin);
}

esp_err_t hub_task_set_pin(const char *pin)
{
    set_pin_call_t call = {.pin = pin, .result = HUB_AUTH_UNAVAILABLE};
    ESP_RETURN_ON_ERROR(run_on_hub_task(set_pin_on_hub, &call), TAG, "setting the PIN");
    if (call.result == HUB_AUTH_OK) {
        return ESP_OK;
    }
    return call.result == HUB_AUTH_INVALID_PIN ? ESP_ERR_INVALID_ARG : ESP_FAIL;
}

bool hub_task_is_alive(void)
{
    return now_ms() - atomic_load(&task_state.last_loop_ms) < HUB_ALIVE_WITHIN_MS;
}

esp_err_t hub_task_restart(hub_restart_level_t level)
{
    /* A hub task that has stopped would never take the event. */
    if (task_state.events == NULL || !hub_task_is_alive()) {
        return ESP_ERR_INVALID_STATE;
    }
    const hub_event_t event = {.type = EVENT_RESTART, .level = level};
    return xQueueSend(task_state.events, &event, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void hub_task_name(char *name, size_t size)
{
    if (xSemaphoreTake(task_state.info_lock, pdMS_TO_TICKS(HUB_INFO_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Hub name locked too long");
        snprintf(name, size, "%s", "");
        return;
    }
    snprintf(name, size, "%s", task_state.name);
    xSemaphoreGive(task_state.info_lock);
}
