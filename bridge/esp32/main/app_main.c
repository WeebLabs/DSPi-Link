/*
 * app_main.c - the bridge's boot sequence, and nothing else.
 *
 * Implements bridge spec 6.5. Each step starts one component; the
 * components hold all behaviour. The LED and button (step 3) and the
 * rollback timer (step 7) join in phase 6 of bridge/esp32/PLAN.md.
 */

#include <inttypes.h>

#include "board.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "http_server.h"
#include "hub_task.h"
#include "net.h"
#include "settings.h"
#include "setup_api.h"

static const char *TAG = "bridge";

/* One block of log lines that says what this bridge is and how it is set
 * up, so a serial log from a user answers the first support questions. */
static void log_startup_summary(const bridge_settings_t *settings)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(NULL, &flash_bytes) != ESP_OK) {
        flash_bytes = 0; /* Reported as 0 MB rather than failing the boot */
    }

    const board_table_t *board = board_table();
    ESP_LOGI(TAG, "DSPi Link bridge %s on %s (revision %d.%d), %" PRIu32 " MB flash, %s",
             esp_app_get_description()->version, board->chip_name, chip.revision / 100,
             chip.revision % 100, flash_bytes / (1024 * 1024),
             board_has_psram() ? "PSRAM" : "no PSRAM");
    ESP_LOGI(TAG, "Hub \"%s\" (%s), hostname %s.local, up to %u sessions", settings->name,
             settings->hub_id, settings->hostname, (unsigned)board_max_sessions());
    ESP_LOGI(TAG, "DSPi UART: TX GPIO %u, RX GPIO %u, %" PRIu32 " baud%s", settings->uart_tx_pin,
             settings->uart_rx_pin, settings->uart_baud,
             settings->uart_detect_baud ? " (automatic detection on)" : "");
}

void app_main(void)
{
    /* 1. Make flash storage usable; everything saved lives there. */
    ESP_ERROR_CHECK(settings_init());

    /* 2. Load the settings. First boot creates the hub id and names. */
    bridge_settings_t settings;
    ESP_ERROR_CHECK(settings_load(&settings));
    log_startup_summary(&settings);

    /* 4. Join Wi-Fi and start mDNS, so the hub has somewhere to advertise. */
    ESP_ERROR_CHECK(net_start(settings.hostname));

    /* 5. Start the hub task, which starts the uart task that feeds it. */
    ESP_ERROR_CHECK(hub_task_start(&settings));

    /* 6. Start the HTTP servers: Link sessions go to the hub task, and the
     *    setup network's requests to the setup API. */
    const http_server_config_t http = {
        .max_sessions = board_max_sessions(),
        .handlers = hub_task_http_handlers(),
        .setup_request = setup_api_handle,
    };
    ESP_ERROR_CHECK(http_server_start(&http));
}
