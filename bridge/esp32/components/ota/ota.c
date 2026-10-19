/*
 * ota.c - firmware updates and rollback (bridge spec 14.2). See ota.h.
 *
 * The ota task takes three kinds of item from one queue, in order: begin an
 * update, write a chunk, abort. Each begin or abort bumps a generation
 * number. Work goes ahead only while its update's generation is still the
 * current one, so once an update is abandoned nothing more of it is
 * written, it is never switched to, and its reports carry a generation the
 * hub no longer accepts.
 */

#include "ota.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "bridge_config.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "psa/crypto.h"

static const char *TAG = "ota";

/* Chunks plus a begin and an abort. */
#define OTA_ITEM_QUEUE_DEPTH (OTA_QUEUE_DEPTH + 2)

typedef enum {
    ITEM_BEGIN,
    ITEM_CHUNK,
    ITEM_ABORT,
} item_kind_t;

typedef struct {
    item_kind_t kind;
    uint32_t generation;
    /* Begin */
    uint32_t size;
    uint8_t sha256[OTA_SHA256_BYTES];
    /* Chunk; `data` is the ota task's to free */
    uint32_t offset;
    uint8_t *data;
    size_t length;
} ota_item_t;

/* The update the ota task is writing. Touched only by the ota task. */
typedef struct {
    bool active;
    uint32_t generation;
    const esp_partition_t *partition;
    esp_ota_handle_t handle;
    psa_hash_operation_t hash;
    uint32_t size;
    uint32_t written;
    uint8_t expected_sha256[OTA_SHA256_BYTES];
} update_t;

static struct {
    ota_handlers_t handlers;
    QueueHandle_t queue;
    atomic_uint generation;     /* Bumped by every begin and abort */
    atomic_uint chunks_waiting; /* Chunks in the queue, for ota_write's limit */
    update_t update;
    /* An image on trial (ota_confirm_after_healthy_start). `healthy` is
     * NULL when there is none. */
    esp_timer_handle_t confirm_timer;
    int64_t trial_started_ms;
    _Atomic(bool (*)(void)) healthy;
    atomic_bool confirmed;
} ota_state;

/* ---------------------------------------------------------------------------
 * The ota task
 * ------------------------------------------------------------------------- */

/* Ends the update in progress without touching the running image. */
static void drop_update(void)
{
    update_t *update = &ota_state.update;
    if (!update->active) {
        return;
    }
    update->active = false;
    const esp_err_t err = esp_ota_abort(update->handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Abandoning the partial image: %s", esp_err_to_name(err));
    }
    if (psa_hash_abort(&update->hash) != PSA_SUCCESS) {
        ESP_LOGW(TAG, "Could not release the image's hash");
    }
}

/* True while `generation` is the update the hub still wants: no abort or
 * newer begin has been asked for since. */
static bool still_wanted(uint32_t generation)
{
    return generation == atomic_load(&ota_state.generation);
}

/* Ends the update in progress as a failure and says why. */
static void fail(const char *why)
{
    ESP_LOGW(TAG, "Update failed: %s", why);
    const uint32_t generation = ota_state.update.generation;
    drop_update();
    ota_state.handlers.finished(ota_state.handlers.context, generation, false, why);
}

static void begin(const ota_item_t *item)
{
    drop_update();
    update_t *update = &ota_state.update;
    *update = (update_t){
        .generation = item->generation,
        .partition = esp_ota_get_next_update_partition(NULL),
        .hash = PSA_HASH_OPERATION_INIT,
        .size = item->size,
    };
    memcpy(update->expected_sha256, item->sha256, OTA_SHA256_BYTES);
    if (!still_wanted(item->generation)) {
        return; /* Abandoned before it began */
    }

    /* A finished update may already be waiting to start (its sender left,
     * or the restart has not happened yet). Writing over the partition the
     * bootloader will start next could leave a wrong image there, so wait
     * for that restart. */
    if (esp_ota_get_boot_partition() != esp_ota_get_running_partition()) {
        ota_state.handlers.finished(
            ota_state.handlers.context, item->generation, false,
            "a new firmware is installed and starts at the next restart; restart the bridge first");
        return;
    }

    /* Sequential writes erase each sector as the image reaches it, rather
     * than the whole image up front, which would stall for seconds. */
    const esp_err_t err =
        update->partition == NULL
            ? ESP_ERR_NOT_FOUND
            : esp_ota_begin(update->partition, OTA_WITH_SEQUENTIAL_WRITES, &update->handle);
    if (err != ESP_OK) {
        /* A firmware still on trial (bridge spec 14.2) must keep the one
         * it would go back to, so ESP-IDF refuses to overwrite it. */
        ota_state.handlers.finished(
            ota_state.handlers.context, item->generation, false,
            err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE
                ? "the bridge has only just been updated; try again in a minute"
                : "the bridge could not prepare its flash for the update");
        return;
    }
    if (psa_hash_setup(&update->hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        if (esp_ota_abort(update->handle) != ESP_OK) {
            ESP_LOGW(TAG, "Could not release the update partition");
        }
        ota_state.handlers.finished(ota_state.handlers.context, item->generation, false,
                                    "the bridge could not check the update");
        return;
    }
    update->active = true;
    ESP_LOGI(TAG, "Receiving a %lu-byte image into %s", (unsigned long)update->size,
             update->partition->label);
}

/* Every byte is in flash: check the image and, if it is good, start it
 * next time. */
static void finish(void)
{
    update_t *update = &ota_state.update;

    uint8_t actual[OTA_SHA256_BYTES];
    size_t actual_length = 0;
    const bool hashed =
        psa_hash_finish(&update->hash, actual, sizeof(actual), &actual_length) == PSA_SUCCESS;
    if (!hashed || memcmp(actual, update->expected_sha256, OTA_SHA256_BYTES) != 0) {
        fail("the file was damaged on the way: its SHA-256 does not match");
        return;
    }

    /* esp_ota_end() checks the image's headers and that it was built for
     * this chip; the handle is closed whatever it finds. */
    const esp_err_t end = esp_ota_end(update->handle);
    update->active = false;
    if (end != ESP_OK) {
        fail(end == ESP_ERR_OTA_VALIDATE_FAILED ? "this file is not firmware for this chip"
                                                : "the bridge could not finish writing the update");
        return;
    }

    /* Any valid image for this chip would pass the checks above; make sure
     * it is the bridge, so a wrong file cannot leave a board without it. */
    esp_app_desc_t description;
    if (esp_ota_get_partition_description(update->partition, &description) != ESP_OK ||
        strcmp(description.project_name, esp_app_get_description()->project_name) != 0) {
        fail("this file is firmware for this chip, but not DSPi bridge firmware");
        return;
    }

    /* The check took a moment; if the update was abandoned meanwhile, the
     * running firmware stays the one that starts. */
    if (!still_wanted(update->generation)) {
        ESP_LOGI(TAG, "Update abandoned during its final check; keeping this firmware");
        return;
    }
    if (esp_ota_set_boot_partition(update->partition) != ESP_OK) {
        fail("the bridge could not switch to the new firmware");
        return;
    }
    ESP_LOGI(TAG, "Firmware %s is ready and starts after the restart", description.version);
    ota_state.handlers.finished(ota_state.handlers.context, update->generation, true,
                                description.version);
}

static void write_chunk(const ota_item_t *item)
{
    update_t *update = &ota_state.update;
    if (!update->active || item->generation != update->generation ||
        !still_wanted(item->generation)) {
        return; /* Left over from an update that was abandoned */
    }

    if (item->offset != update->written || item->length > update->size - update->written) {
        fail("the image arrived out of order");
        return;
    }
    /* The first write checks the image's first byte, so a file that is not
     * firmware at all is refused here. */
    const esp_err_t err = esp_ota_write(update->handle, item->data, item->length);
    if (err != ESP_OK) {
        fail(err == ESP_ERR_OTA_VALIDATE_FAILED ? "this file is not firmware for this chip"
                                                : "the bridge could not write the update to flash");
        return;
    }
    if (psa_hash_update(&update->hash, item->data, item->length) != PSA_SUCCESS) {
        fail("the bridge could not check the update");
        return;
    }
    ota_state.handlers.written(ota_state.handlers.context, update->generation, item->offset);

    update->written += (uint32_t)item->length;
    if (update->written == update->size) {
        finish();
    }
}

static void ota_task(void *argument)
{
    (void)argument;
    for (;;) {
        /* The task's only job is this queue, so it may wait for ever: there
         * is nothing else for it to notice while no update is running. A
         * stalled update is abandoned by the hub (OTA_IDLE_TIMEOUT_MS). */
        ota_item_t item;
        if (xQueueReceive(ota_state.queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (item.kind) {
        case ITEM_BEGIN:
            begin(&item);
            break;
        case ITEM_CHUNK:
            /* Off the queue now, so its place is free before its ack goes
             * out; the client may send the next chunk the moment it hears. */
            atomic_fetch_sub(&ota_state.chunks_waiting, 1);
            write_chunk(&item);
            free(item.data);
            break;
        case ITEM_ABORT:
            if (ota_state.update.active) {
                ESP_LOGI(TAG, "Update abandoned");
            }
            drop_update();
            break;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public functions
 * ------------------------------------------------------------------------- */

esp_err_t ota_start(const ota_handlers_t *handlers)
{
    ota_state.handlers = *handlers;
    ota_state.queue = xQueueCreate(OTA_ITEM_QUEUE_DEPTH, sizeof(ota_item_t));
    if (ota_state.queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(ota_task, "ota", OTA_TASK_STACK_BYTES, NULL, OTA_TASK_PRIORITY, NULL) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint32_t ota_capacity(void)
{
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    return partition != NULL ? partition->size : 0;
}

uint32_t ota_begin(uint32_t size, const uint8_t sha256[OTA_SHA256_BYTES])
{
    if (size == 0 || size > ota_capacity()) {
        return 0;
    }
    uint32_t generation = atomic_fetch_add(&ota_state.generation, 1) + 1;
    if (generation == 0) {
        generation = atomic_fetch_add(&ota_state.generation, 1) + 1; /* 0 means failure */
    }
    ota_item_t item = {.kind = ITEM_BEGIN, .generation = generation, .size = size};
    memcpy(item.sha256, sha256, OTA_SHA256_BYTES);
    return xQueueSend(ota_state.queue, &item, 0) == pdTRUE ? generation : 0;
}

bool ota_write(uint32_t offset, const uint8_t *data, size_t length)
{
    if (atomic_load(&ota_state.chunks_waiting) >= OTA_QUEUE_DEPTH) {
        return false;
    }
    ota_item_t item = {
        .kind = ITEM_CHUNK,
        .generation = atomic_load(&ota_state.generation),
        .offset = offset,
        .data = malloc(length),
        .length = length,
    };
    if (item.data == NULL) {
        return false;
    }
    memcpy(item.data, data, length);
    atomic_fetch_add(&ota_state.chunks_waiting, 1);
    if (xQueueSend(ota_state.queue, &item, 0) != pdTRUE) {
        atomic_fetch_sub(&ota_state.chunks_waiting, 1);
        free(item.data);
        return false;
    }
    return true;
}

void ota_abort(void)
{
    ota_item_t item = {
        .kind = ITEM_ABORT,
        .generation = atomic_fetch_add(&ota_state.generation, 1) + 1,
    };
    /* The bumped generation alone already stops the update: its chunks are
     * no longer written and its image is never switched to. The item only
     * releases the partition sooner. */
    if (xQueueSend(ota_state.queue, &item, pdMS_TO_TICKS(OTA_ABORT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "The ota queue is full; the abandoned update's chunks are dropped");
    }
}

/* ---------------------------------------------------------------------------
 * Confirming a new image
 * ------------------------------------------------------------------------- */

_Static_assert(OTA_CONFIRM_GIVE_UP_MS > WIFI_FALLBACK_AFTER_MS + OTA_CONFIRM_RETRY_MS,
               "a home network that is down must give way to the setup network first");

/* Writes the confirmation, once. True when the image is confirmed. Called
 * from the timer task and, through ota_confirm_now_if_healthy(), from
 * whichever task restarts the bridge. */
static bool confirm_image(const char *why)
{
    if (atomic_load(&ota_state.confirmed)) {
        return true;
    }
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not confirm this firmware: %s", esp_err_to_name(err));
        return false;
    }
    atomic_store(&ota_state.confirmed, true);
    ESP_LOGI(TAG, "This firmware is confirmed: %s", why);
    return true;
}

static void check_trial(void *argument)
{
    (void)argument;
    if (atomic_load(&ota_state.confirmed)) {
        return;
    }
    const int64_t running_ms = esp_timer_get_time() / 1000 - ota_state.trial_started_ms;
    const bool healthy = atomic_load(&ota_state.healthy)();
    if (healthy && confirm_image("it is running properly")) {
        return;
    }
    if (!healthy && running_ms >= OTA_CONFIRM_GIVE_UP_MS) {
        ESP_LOGE(TAG, "This firmware is not running properly; returning to the previous one");
        /* Returns only if it could not restart. */
        const esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(TAG, "Could not return to the previous firmware: %s", esp_err_to_name(err));
        return;
    }
    if (!healthy) {
        ESP_LOGW(TAG, "This firmware is not running properly yet; checking again in %d s",
                 OTA_CONFIRM_RETRY_MS / 1000);
    }
    /* Unhealthy so far, or healthy but the confirmation was not written. */
    const esp_err_t err =
        esp_timer_start_once(ota_state.confirm_timer, (uint64_t)OTA_CONFIRM_RETRY_MS * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not check this firmware again (%s); the next restart rolls back",
                 esp_err_to_name(err));
    }
}

void ota_confirm_after_healthy_start(bool (*healthy)(void))
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return; /* Confirmed already, or flashed by cable */
    }

    ESP_LOGI(TAG, "New firmware: it is confirmed once it runs properly, from %d s on",
             OTA_CONFIRM_AFTER_MS / 1000);
    ota_state.trial_started_ms = esp_timer_get_time() / 1000;
    atomic_store(&ota_state.healthy, healthy);
    const esp_timer_create_args_t timer = {.callback = check_trial, .name = "ota_confirm"};
    if (esp_timer_create(&timer, &ota_state.confirm_timer) != ESP_OK ||
        esp_timer_start_once(ota_state.confirm_timer, (uint64_t)OTA_CONFIRM_AFTER_MS * 1000) !=
            ESP_OK) {
        /* Without the timer the image would never be confirmed and the next
         * restart would roll back; confirm now rather than lose it. */
        ESP_LOGW(TAG, "Could not start the confirmation timer; confirming now");
        (void)confirm_image("its checks could not be scheduled");
    }
}

void ota_confirm_now_if_healthy(void)
{
    bool (*healthy)(void) = atomic_load(&ota_state.healthy);
    if (healthy == NULL || atomic_load(&ota_state.confirmed)) {
        return; /* No image on trial */
    }
    if (healthy()) {
        (void)confirm_image("it was running properly when a restart was asked for");
    }
}
