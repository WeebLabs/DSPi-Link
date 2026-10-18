/*
 * phase2_bench.c - TEMPORARY. See phase2_bench.h; removed in phase 3.
 *
 * Every handler runs on the uart task, so each one only logs and returns.
 */

#include "phase2_bench.h"

#include <inttypes.h>

#include "dspi_uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "settings.h"

static const char *TAG = "bench";

/* REQ_GET_ALL_PARAMS: the whole parameter blob, 6136 bytes at wire V32. */
#define REQ_GET_ALL_PARAMS 0xA0
#define BULK_READ_TAG 1

static bridge_settings_t saved_settings;
static int64_t bulk_started_us;

static void on_device_found(void *context, const dspi_identity_t *identity, uint32_t baud)
{
    (void)context;
    const uint8_t *platform = identity->platform;
    if (identity->platform_length >= 7) {
        /* Bytes: platform, major, legacy nibbles, outputs, minor, patch, beta. */
        ESP_LOGI(TAG, "DSPi %s: %s, firmware %u.%u.%u, beta %u, %u outputs", identity->serial,
                 platform[0] == 1 ? "RP2350" : "RP2040", platform[1], platform[4], platform[5],
                 platform[6], platform[3]);
    } else {
        ESP_LOGI(TAG, "DSPi %s: platform reply of %u bytes (older firmware)", identity->serial,
                 identity->platform_length);
    }

    if (!identity->has_uart_config) {
        ESP_LOGW(TAG, "Could not read the DSPi's UART configuration (0xF6)");
    } else if (!identity->notify_enabled) {
        ESP_LOGW(TAG, "Notifications are off on the DSPi: turn them on in DSPi Console");
    } else {
        ESP_LOGI(TAG, "Notifications are on; the DSPi's UART is set to %" PRIu32 " baud",
                 identity->configured_baud);
    }

    if (baud != saved_settings.uart_baud) {
        saved_settings.uart_baud = baud;
        const esp_err_t err = settings_save(&saved_settings);
        ESP_LOGI(TAG, "Saved the detected rate of %" PRIu32 " baud (%s)", baud,
                 esp_err_to_name(err));
    }

    /* One bulk read shows the largest transfer working end to end. */
    const dspi_request_t bulk = {.is_get = true, .request = REQ_GET_ALL_PARAMS, .length = 0};
    bulk_started_us = esp_timer_get_time();
    if (dspi_uart_submit(&bulk, true, BULK_READ_TAG) != ESP_OK) {
        ESP_LOGE(TAG, "Could not queue the bulk read");
    }
}

static void on_device_lost(void *context)
{
    (void)context;
    ESP_LOGW(TAG, "DSPi lost; looking for it again");
}

static void on_notification(void *context, const uint8_t *packet, size_t length)
{
    (void)context;
    ESP_LOGI(TAG, "Notification, %u bytes:", (unsigned)length);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, packet, length, ESP_LOG_INFO);
}

static void on_reply(void *context, uint32_t tag, const dspi_reply_t *reply)
{
    (void)context;
    if (tag != BULK_READ_TAG) {
        return;
    }
    const int64_t elapsed_ms = (esp_timer_get_time() - bulk_started_us) / 1000;
    if (reply->kind == DSPI_REPLY_ANSWERED && reply->status == 0) {
        ESP_LOGI(TAG, "Bulk read: %u bytes in %" PRId64 " ms after %u attempt(s)",
                 (unsigned)reply->length, elapsed_ms, reply->attempts);
    } else {
        ESP_LOGW(TAG, "Bulk read failed: kind %d, status 0x%02X, after %" PRId64 " ms",
                 (int)reply->kind, reply->status, elapsed_ms);
    }
}

esp_err_t phase2_bench_start(const bridge_settings_t *settings)
{
    saved_settings = *settings;
    const dspi_uart_config_t config = {
        .tx_pin = settings->uart_tx_pin,
        .rx_pin = settings->uart_rx_pin,
        .baud = settings->uart_baud,
        .detect_baud = settings->uart_detect_baud,
        .handlers =
            {
                .device_found = on_device_found,
                .device_lost = on_device_lost,
                .notification = on_notification,
                .reply = on_reply,
            },
    };
    return dspi_uart_start(&config);
}
