/*
 * ota.h - firmware updates and rollback (bridge spec 14.2).
 *
 * An image arrives in chunks, in order. Each chunk is copied into a short
 * queue and written to the inactive app partition by the ota task, which
 * also hashes it, so the caller (the hub task) never waits on flash. When
 * the last byte is written the task checks the SHA-256, lets ESP-IDF check
 * the image (it must be built for this chip), checks that it is bridge
 * firmware, and makes it the image that starts next.
 *
 * The caller hears back through two handlers, called on the ota task:
 * `written` once per chunk in flash, then `finished` once per update. A
 * chunk that cannot be written gets no `written`; `finished` says why.
 *
 * Every function may be called from any task.
 */

#ifndef OTA_H
#define OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define OTA_SHA256_BYTES 32

/* Each report names the update it belongs to by the generation ota_begin()
 * returned, so a report from an abandoned update is never taken for the
 * next one's. */
typedef struct {
    void *context;
    /* The chunk at `offset` is in flash. */
    void (*written)(void *context, uint32_t generation, uint32_t offset);
    /* The update is over. `text` is the new image's version when `ok`,
     * otherwise a sentence saying what went wrong. */
    void (*finished)(void *context, uint32_t generation, bool ok, const char *text);
} ota_handlers_t;

/* Starts the ota task. Call once. */
esp_err_t ota_start(const ota_handlers_t *handlers);

/* The largest image the inactive partition holds, in bytes. */
uint32_t ota_capacity(void);

/* Starts receiving an image of `size` bytes whose SHA-256 is `sha256`,
 * abandoning any update in progress. Returns the update's generation, never
 * 0, or 0 if `size` does not fit. */
uint32_t ota_begin(uint32_t size, const uint8_t sha256[OTA_SHA256_BYTES]);

/* Queues the chunk at `offset`, copying it. False when OTA_QUEUE_DEPTH
 * chunks are already waiting or memory ran out. */
bool ota_write(uint32_t offset, const uint8_t *data, size_t length);

/* Abandons the update in progress, if any. Chunks still queued are
 * dropped, and an image being checked is not switched to. The running
 * image is untouched. */
void ota_abort(void);

/*
 * Step 7 of the boot sequence (bridge spec 6.5). If this image is new and
 * not yet confirmed, asks `healthy` OTA_CONFIRM_AFTER_MS after the start
 * and then every OTA_CONFIRM_RETRY_MS: the first true confirms the image
 * (a confirmation that cannot be written is tried again). Still false
 * OTA_CONFIRM_GIVE_UP_MS after the start, the previous image is restored
 * at once. A crash or restart before confirmation also returns to the
 * previous image, because the bootloader sees it still unconfirmed.
 * `healthy` runs on ESP-IDF's timer task. Called once, by app_main.
 */
void ota_confirm_after_healthy_start(bool (*healthy)(void));

/* Confirms an image on trial at once if `healthy` says so; does nothing
 * otherwise, or when no image is on trial. For a restart the user asked
 * for, which would otherwise return to the previous firmware. Any task. */
void ota_confirm_now_if_healthy(void);

#endif /* OTA_H */
