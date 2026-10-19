/*
 * app_main.c - the bridge's boot sequence, and nothing else.
 *
 * Implements bridge spec 6.5. Each step starts one component; the
 * components hold all behaviour. The few small functions above app_main()
 * connect components that do not know each other: the button to the hub,
 * the network and DSPi state to the LED, and every task to the checks that
 * keep a new firmware and restart a stuck bridge (bridge spec 12.3, 14.2).
 */

#include <inttypes.h>

#include "board.h"
#include "bridge_config.h"
#include "button.h"
#include "dspi_uart.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_server.h"
#include "hub_task.h"
#include "net.h"
#include "ota.h"
#include "settings.h"
#include "setup_api.h"
#include "status_led.h"

static const char *TAG = "bridge";

/* Why the chip last started, in words, for the startup summary (bridge
 * spec 12.3). */
static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "power on";
    case ESP_RST_EXT:
        return "reset pin";
    case ESP_RST_SW:
        return "restart by the firmware";
    case ESP_RST_PANIC:
        return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_BROWNOUT:
        return "brownout (power supply too weak)";
    case ESP_RST_DEEPSLEEP:
        return "deep sleep";
    case ESP_RST_SDIO:
        return "SDIO";
    case ESP_RST_USB:
        return "USB";
    case ESP_RST_JTAG:
        return "JTAG";
    case ESP_RST_EFUSE:
        return "eFuse error";
    case ESP_RST_PWR_GLITCH:
        return "power glitch";
    case ESP_RST_CPU_LOCKUP:
        return "CPU lockup";
    case ESP_RST_UNKNOWN:
        break;
    }
    return "unknown";
}

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
    ESP_LOGI(TAG, "Started by: %s", reset_reason_name(esp_reset_reason()));
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
    /* A restart the user asked for is no sign of a bad firmware. */
    ota_confirm_now_if_healthy();
    esp_restart();
}

/* True while the hub task and the uart task both go round their loops. */
static bool tasks_running(void)
{
    return hub_task_is_alive() && dspi_uart_is_alive();
}

/* A new firmware is kept once it runs properly (bridge spec 14.2): every
 * task going round its loop, the bridge reachable, on its home network
 * with an address or on its setup network, and the Link server running,
 * which it is once step 7 has run, or the boot would have stopped there. */
static bool running_properly(void)
{
    net_status_t network;
    net_get_status(&network);
    const bool reachable = (network.connected && network.ip[0] != '\0') || network.setup_network_on;
    return tasks_running() && reachable;
}

/* Restarts the bridge when the hub task or the uart task has stopped going
 * round its loop for SUPERVISOR_RESTART_AFTER_MS (bridge spec 12.3). Runs
 * on ESP-IDF's timer task every SUPERVISOR_CHECK_MS. */
static void supervise(void *argument)
{
    (void)argument;
    static int64_t stuck_since_ms = -1;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (tasks_running()) {
        stuck_since_ms = -1;
        return;
    }
    if (stuck_since_ms < 0) {
        stuck_since_ms = now_ms;
        ESP_LOGW(TAG, "The %s task has stopped; restarting in %d s unless it recovers",
                 hub_task_is_alive() ? "uart" : "hub", SUPERVISOR_RESTART_AFTER_MS / 1000);
        return;
    }
    if (now_ms - stuck_since_ms >= SUPERVISOR_RESTART_AFTER_MS) {
        ESP_LOGE(TAG, "The %s task is stuck; restarting the bridge",
                 hub_task_is_alive() ? "uart" : "hub");
        esp_restart();
    }
}

static esp_err_t start_supervisor(void)
{
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {.callback = supervise, .name = "supervisor"};
    esp_err_t err = esp_timer_create(&args, &timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(timer, (uint64_t)SUPERVISOR_CHECK_MS * 1000);
    }
    return err;
}

/* A boot step failed, so the bridge cannot work. Restarting at once would
 * loop faster than anyone could hold BOOT, so wait with the button watched,
 * then restart; a firmware on trial then returns to the previous one. */
static void boot_failed(const char *step, esp_err_t err)
{
    ESP_LOGE(TAG,
             "Could not %s: %s. Restarting in %d s; hold BOOT for 15 to 30 s for a "
             "factory reset",
             step, esp_err_to_name(err), BOOT_FAILURE_RESTART_MS / 1000);
    vTaskDelay(pdMS_TO_TICKS(BOOT_FAILURE_RESTART_MS));
    esp_restart();
}

/* Runs one boot step and stops the boot if it fails. */
static void must(esp_err_t err, const char *step)
{
    if (err != ESP_OK) {
        boot_failed(step, err);
    }
}

void app_main(void)
{
    /* 1. Watch the BOOT button first, so a factory reset works even when a
     *    later step fails (bridge spec 12.1). */
    const button_handlers_t button = {.stage_changed = on_button_stage, .action = on_button_action};
    must(button_start(board_table()->boot_button_pin, &button), "watch the BOOT button");

    /* 2. Make flash storage usable; everything saved lives there. */
    must(settings_init(), "open the flash storage");

    /* 3. Load the settings. First boot creates the hub id and names. */
    bridge_settings_t settings;
    must(settings_load(&settings), "load the settings");
    log_startup_summary(&settings);

    /* 4. Start the status LED. */
    must(status_led_start(settings.led_type, settings.led_pin, read_led_inputs),
         "start the status LED");

    /* 5. Join Wi-Fi and start mDNS, so the hub has somewhere to advertise.
     *    tools/qemu_smoke.sh stops at this log line, since QEMU has no
     *    radio to start. */
    ESP_LOGI(TAG, "Starting the network");
    must(net_start(settings.hostname), "start Wi-Fi");

    /* 6. Start the hub task, which starts the uart task that feeds it. */
    must(hub_task_start(&settings), "start the hub");

    /* 7. Start the HTTP servers: Link sessions go to the hub task, and the
     *    setup network's requests to the setup API. */
    const http_server_config_t http = {
        .max_sessions = board_max_sessions(),
        .handlers = hub_task_http_handlers(),
        .setup_request = setup_api_handle,
    };
    must(http_server_start(&http), "start the HTTP servers");

    /* 8. Restart the bridge if the hub task or the uart task gets stuck. */
    must(start_supervisor(), "start the supervisor");
    ESP_LOGI(TAG, "Bridge started");

    /* 9. If this firmware has just been installed, keep it only once it
     *    runs properly. */
    ota_confirm_after_healthy_start(running_properly);
}
