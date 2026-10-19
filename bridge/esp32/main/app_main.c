/*
 * app_main.c - the bridge's boot sequence, and nothing else.
 *
 * Implements bridge spec 6.5. Each step starts one component; the
 * components hold all behaviour. The few small functions above app_main()
 * connect components that do not know each other: the button to the hub,
 * and the network and DSPi state to the LED.
 */

#include <inttypes.h>

#include "board.h"
#include "button.h"
#include "dspi_uart.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "http_server.h"
#include "hub_task.h"
#include "net.h"
#include "ota.h"
#include "settings.h"
#include "setup_api.h"
#include "status_led.h"

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

/* What the LED reports: the network's state and whether the DSPi answers. */
static void read_led_inputs(led_inputs_t *inputs)
{
    net_status_t network;
    net_get_status(&network);
    switch (network.state) {
    case NET_STATE_SETUP:
    case NET_STATE_FALLBACK:
        inputs->network = LED_NETWORK_SETUP;
        break;
    case NET_STATE_CONNECTING:
        inputs->network = LED_NETWORK_CONNECTING;
        break;
    case NET_STATE_ONLINE:
        inputs->network = LED_NETWORK_ONLINE;
        break;
    }
    dspi_uart_status_t link;
    dspi_uart_get_status(&link);
    inputs->device_present = link.device_present;
}

static void on_button_stage(void *context, button_stage_t stage)
{
    (void)context;
    status_led_show_button(stage);
}

/* A long press of BOOT does what hub.reset does (bridge spec 12.1). The
 * hub task normally does it, telling every session first; if it cannot
 * take the request (stopped, or its queue full), the button must still
 * work, so this erases and restarts by itself. */
static void on_button_action(void *context, button_action_t action)
{
    (void)context;
    const bool factory = action == BUTTON_ACTION_FACTORY_RESET;
    const esp_err_t asked = hub_task_restart(factory ? HUB_RESTART_FACTORY : HUB_RESTART_NETWORK);
    if (asked == ESP_OK) {
        return;
    }
    ESP_LOGW(TAG, "The hub task could not take the reset (%s); doing it here",
             esp_err_to_name(asked));
    const esp_err_t erased = factory ? settings_erase_all() : settings_forget_wifi();
    if (erased != ESP_OK) {
        ESP_LOGE(TAG, "Could not erase before restarting: %s", esp_err_to_name(erased));
    }
    esp_restart();
}

/* A new firmware is kept if, after a minute, the hub task and the uart task
 * are both still running (bridge spec 14.2). Wi-Fi started in step 4, or
 * the boot would not have got this far. */
static bool running_properly(void)
{
    return hub_task_is_alive() && dspi_uart_is_alive();
}

void app_main(void)
{
    /* 1. Make flash storage usable; everything saved lives there. */
    ESP_ERROR_CHECK(settings_init());

    /* 2. Load the settings. First boot creates the hub id and names. */
    bridge_settings_t settings;
    ESP_ERROR_CHECK(settings_load(&settings));
    log_startup_summary(&settings);

    /* 3. Start the status LED and the button watcher. */
    ESP_ERROR_CHECK(status_led_start(settings.led_type, settings.led_pin, read_led_inputs));
    const button_handlers_t button = {.stage_changed = on_button_stage, .action = on_button_action};
    ESP_ERROR_CHECK(button_start(board_table()->boot_button_pin, &button));

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

    /* 7. If this firmware has just been installed, keep it only if it runs
     *    properly for a minute. */
    ota_confirm_after_healthy_start(running_properly);
}
