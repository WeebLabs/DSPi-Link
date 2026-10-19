/*
 * dspi_uart.c - the uart task and the ESP32 UART it drives.
 *
 * Implements bridge spec 6.2 and 7.5 to 7.7. The protocol work (timing,
 * retries, quarantine, identity) is in the portable dspi_link.c; this file
 * supplies the real UART as a dspi_port_t and runs the task's loop:
 *
 *   no device:  probe every DSPI_UART_PROBE_INTERVAL_MS, answering any
 *               queued request with DSPI_REPLY_NO_DEVICE
 *   device:     run queued requests in order; while idle, read
 *               notifications and send a heartbeat after
 *               DSPI_UART_HEARTBEAT_IDLE_MS
 */

#include "dspi_uart.h"

#include <string.h>

#include "bridge_config.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "dspi_uart";

#define UART_PORT ((uart_port_t)DSPI_UART_PORT_NUMBER)

/* One queued item: a request for the DSPi, or new link settings. */
typedef struct {
    bool reconfigure; /* True: apply `settings`; false: send `request` */
    dspi_request_t request;
    bool may_retry;
    uint32_t tag;
    dspi_uart_config_t settings; /* Pins, rate and detection; handlers unused */
} queued_request_t;

/* Everything the uart task owns. Only the task touches it after start,
 * except `queue`, which FreeRTOS makes safe to send to from any task, and
 * the two fields other tasks read: `published`, under `status_lock`, and
 * `last_loop_ms`, a single 32-bit word. */
static struct {
    dspi_uart_config_t config;
    dspi_link_t link;
    bool driver_installed;
    bool device_present;
    dspi_identity_t identity; /* Of the device present */
    bool identity_warned;     /* An unreadable identity was logged; not again until found */
    QueueHandle_t queue;
    volatile uint32_t last_loop_ms; /* For dspi_uart_is_alive() */
    dspi_uart_status_t published;   /* For dspi_uart_get_status(), counters aside */
} uart_task_state;

/* Guards `published`, which the uart task writes and others copy. */
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;

/* Holds one received frame, up to a whole bulk blob. Static so the task
 * stack stays small. */
static uint8_t receive_buffer[DSPI_PARSER_BUFFER_BYTES(LINK_MAX_PAYLOAD_BYTES)];

/* ---------------------------------------------------------------------------
 * The ESP32 UART as a dspi_port_t
 * ------------------------------------------------------------------------- */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static esp_err_t install_uart(uint8_t tx_pin, uint8_t rx_pin, uint32_t baud)
{
    const uart_config_t settings = {
        .baud_rate = (int)baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, /* 8N1: the frame CRC guards integrity */
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* The interrupt lives in IRAM (CONFIG_UART_ISR_IN_IRAM), so it keeps
     * emptying the 128-byte hardware FIFO while flash is written, during an
     * update or a settings save. Otherwise bytes are lost within 1.3 ms at
     * 1 Mbaud. */
    esp_err_t err = uart_driver_install(UART_PORT, DSPI_UART_DRIVER_RX_BUFFER_BYTES, 0, 0, NULL,
                                        ESP_INTR_FLAG_IRAM);
    if (err == ESP_OK) {
        err = uart_param_config(UART_PORT, &settings);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(UART_PORT, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        /* Keeps an unconnected RX line high (idle) rather than floating,
         * which would read as noise. */
        err = gpio_pullup_en((gpio_num_t)rx_pin);
    }
    return err;
}

/* Removes the driver if it is installed, then installs it afresh. On
 * failure the driver is left removed; uart_task_state.driver_installed
 * says which. */
static esp_err_t reinstall_uart(uint8_t tx_pin, uint8_t rx_pin, uint32_t baud)
{
    if (uart_task_state.driver_installed) {
        const esp_err_t err = uart_driver_delete(UART_PORT);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Removing the UART driver failed: %s", esp_err_to_name(err));
        }
        uart_task_state.driver_installed = false;
    }
    esp_err_t err = install_uart(tx_pin, rx_pin, baud);
    if (err != ESP_OK && uart_is_driver_installed(UART_PORT)) {
        /* Installed but not configured: remove it so a retry starts clean. */
        (void)uart_driver_delete(UART_PORT);
    }
    uart_task_state.driver_installed = err == ESP_OK;
    return err;
}

static bool port_write(void *context, const uint8_t *data, size_t length)
{
    (void)context;
    if (!uart_task_state.driver_installed) {
        return false;
    }
    /* With no TX ring buffer this returns once the last byte is in the
     * hardware FIFO. Its wait is bounded by the payload's wire time (about
     * 710 ms for 8 KB at 115200): flow control is off, so the transmitter
     * never stalls. Only the FIFO's tail is left for uart_wait_tx_done. */
    if (uart_write_bytes(UART_PORT, data, length) != (int)length) {
        return false;
    }
    return uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(DSPI_UART_TX_DONE_TIMEOUT_MS)) == ESP_OK;
}

/* How many received bytes the driver holds; 0 if it cannot say. */
static size_t buffered_bytes(void)
{
    size_t buffered = 0;
    if (uart_get_buffered_data_len(UART_PORT, &buffered) != ESP_OK) {
        return 0;
    }
    return buffered;
}

static size_t port_read(void *context, uint8_t *data, size_t capacity, uint32_t timeout_ms)
{
    (void)context;
    if (!uart_task_state.driver_installed || capacity == 0) {
        /* Nothing can arrive. Wait anyway, so callers looping until a
         * deadline sleep rather than spin. */
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return 0;
    }
    size_t buffered = buffered_bytes();

    if (buffered == 0) {
        /* uart_read_bytes waits for all the bytes asked for, so ask for one
         * to return as soon as anything arrives. */
        const int got = uart_read_bytes(UART_PORT, data, 1, pdMS_TO_TICKS(timeout_ms));
        if (got <= 0) {
            return 0;
        }
        buffered = buffered_bytes();
        const size_t more = buffered < capacity - 1 ? buffered : capacity - 1;
        const int extra = more > 0 ? uart_read_bytes(UART_PORT, data + 1, more, 0) : 0;
        return 1 + (size_t)(extra > 0 ? extra : 0);
    }

    const size_t take = buffered < capacity ? buffered : capacity;
    const int got = uart_read_bytes(UART_PORT, data, take, 0);
    return got > 0 ? (size_t)got : 0;
}

static bool port_set_baud(void *context, uint32_t baud)
{
    (void)context;
    if (!uart_task_state.driver_installed) {
        return false;
    }
    return uart_set_baudrate(UART_PORT, baud) == ESP_OK && uart_flush_input(UART_PORT) == ESP_OK;
}

static bool port_restart(void *context)
{
    (void)context;
    ESP_LOGW(TAG, "Reinstalling the UART driver after repeated unanswered requests");
    const dspi_uart_config_t *config = &uart_task_state.config;
    const esp_err_t err =
        reinstall_uart(config->tx_pin, config->rx_pin, dspi_link_baud(&uart_task_state.link));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Reinstalling the UART driver failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static uint32_t port_now_ms(void *context)
{
    (void)context;
    return now_ms();
}

/* ---------------------------------------------------------------------------
 * The task
 * ------------------------------------------------------------------------- */

/* Copies what dspi_uart_get_status() reports into `published`. Called by
 * the uart task whenever the pins, rate, presence or identity change. */
static void publish_status(void)
{
    dspi_uart_status_t status = {
        .tx_pin = uart_task_state.config.tx_pin,
        .rx_pin = uart_task_state.config.rx_pin,
        .baud = dspi_link_baud(&uart_task_state.link),
        .device_present = uart_task_state.device_present,
        .identity = uart_task_state.identity,
    };
    portENTER_CRITICAL(&status_lock);
    uart_task_state.published = status;
    portEXIT_CRITICAL(&status_lock);
}

static void relay_notification(void *context, const uint8_t *packet, size_t length)
{
    (void)context;
    if (!uart_task_state.device_present) {
        /* Found but not yet reported, or lost: clients know of no device
         * yet, and read its state once it is reported found. */
        return;
    }
    const dspi_uart_handlers_t *handlers = &uart_task_state.config.handlers;
    if (handlers->notification != NULL) {
        handlers->notification(handlers->context, packet, length);
    }
}

/* Installs the driver on `settings` and moves the link to its rate.
 * Returns false, with the driver removed, if either step fails. */
static bool move_link(const dspi_uart_config_t *settings)
{
    const esp_err_t err = reinstall_uart(settings->tx_pin, settings->rx_pin, settings->baud);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART setup on TX %u, RX %u failed: %s", settings->tx_pin, settings->rx_pin,
                 esp_err_to_name(err));
        return false;
    }
    if (!dspi_link_set_baud(&uart_task_state.link, settings->baud)) {
        ESP_LOGE(TAG, "Setting the UART to %lu baud failed", (unsigned long)settings->baud);
        return false;
    }
    return true;
}

static void report_device_lost(void)
{
    if (!uart_task_state.device_present) {
        return;
    }
    uart_task_state.device_present = false;
    const dspi_uart_handlers_t *handlers = &uart_task_state.config.handlers;
    if (handlers->device_lost != NULL) {
        handlers->device_lost(handlers->context);
    }
}

/* Moves the UART to new pins and rate. Whatever device was there is
 * reported lost; the task looks for it again on the new link. If the new
 * pins cannot be set up, the old ones are restored so the link keeps
 * working as before. */
static void apply_reconfigure(const dspi_uart_config_t *settings)
{
    dspi_uart_config_t *config = &uart_task_state.config;
    const bool same_link = settings->tx_pin == config->tx_pin &&
                           settings->rx_pin == config->rx_pin &&
                           settings->baud == dspi_link_baud(&uart_task_state.link);
    if (same_link && uart_task_state.driver_installed) {
        /* Already running like this: the usual case is the hub saving a rate
         * that detection has just found. Nothing to move, nothing lost. */
        config->baud = settings->baud;
        config->detect_baud = settings->detect_baud;
        return;
    }

    report_device_lost();
    const dspi_uart_config_t previous = *config;
    if (move_link(settings)) {
        config->tx_pin = settings->tx_pin;
        config->rx_pin = settings->rx_pin;
        config->baud = settings->baud;
        config->detect_baud = settings->detect_baud;
        ESP_LOGI(TAG, "Link moved to TX GPIO %u, RX GPIO %u, %lu baud", config->tx_pin,
                 config->rx_pin, (unsigned long)config->baud);
    } else if (move_link(&previous)) {
        ESP_LOGW(TAG, "Kept the link on TX GPIO %u, RX GPIO %u", previous.tx_pin, previous.rx_pin);
    } else {
        /* look_for_device() retries the install on every probe. */
        ESP_LOGE(TAG, "The UART is down; retrying every %d ms", DSPI_UART_PROBE_INTERVAL_MS);
    }
    publish_status();
}

static void deliver_reply(const queued_request_t *item, const dspi_reply_t *reply)
{
    const dspi_uart_handlers_t *handlers = &uart_task_state.config.handlers;
    if (handlers->reply != NULL) {
        handlers->reply(handlers->context, item->tag, reply);
    }
}

/* Answers every queued request with DSPI_REPLY_NO_DEVICE, waiting up to
 * `wait_ms` for the first. Used while no DSPi is present. */
static void refuse_requests(uint32_t wait_ms)
{
    const dspi_reply_t no_device = {.kind = DSPI_REPLY_NO_DEVICE};
    queued_request_t item;
    TickType_t wait = pdMS_TO_TICKS(wait_ms);
    while (xQueueReceive(uart_task_state.queue, &item, wait) == pdTRUE) {
        if (item.reconfigure) {
            apply_reconfigure(&item.settings);
            return; /* Look on the new link straight away */
        }
        deliver_reply(&item, &no_device);
        wait = 0;
    }
}

/* Installs the driver again after a failed reconfigure or restart.
 * Returns true once it is installed. */
static bool ensure_driver(void)
{
    if (uart_task_state.driver_installed) {
        return true;
    }
    /* Quiet on failure: the failure that took the driver down was logged,
     * and this runs on every probe. */
    const dspi_uart_config_t *config = &uart_task_state.config;
    if (reinstall_uart(config->tx_pin, config->rx_pin, config->baud) != ESP_OK ||
        !dspi_link_set_baud(&uart_task_state.link, config->baud)) {
        return false;
    }
    ESP_LOGI(TAG, "UART back up on TX GPIO %u, RX GPIO %u", config->tx_pin, config->rx_pin);
    return true;
}

/* Probes for the DSPi once (bridge spec 7.5 and 7.6). */
static void look_for_device(void)
{
    static const uint32_t candidates[] = DSPI_UART_BAUD_CANDIDATES;
    const dspi_uart_config_t *config = &uart_task_state.config;
    dspi_link_t *link = &uart_task_state.link;

    if (!ensure_driver()) {
        return;
    }
    const size_t candidate_count =
        config->detect_baud ? sizeof(candidates) / sizeof(candidates[0]) : 0;
    const bool answered =
        dspi_link_find_baud(link, dspi_link_baud(link), candidates, candidate_count);
    publish_status(); /* The rate may have changed */
    if (!answered) {
        return;
    }

    dspi_identity_t identity;
    if (!dspi_link_read_identity(link, &identity)) {
        if (!uart_task_state.identity_warned) {
            ESP_LOGW(TAG, "A DSPi answered at %lu baud but its identity could not be read",
                     (unsigned long)dspi_link_baud(link));
            uart_task_state.identity_warned = true;
        }
        return;
    }

    uart_task_state.identity = identity;
    uart_task_state.device_present = true;
    uart_task_state.identity_warned = false;
    publish_status();
    ESP_LOGI(TAG, "Found DSPi %s at %lu baud", identity.serial,
             (unsigned long)dspi_link_baud(link));
    if (config->handlers.device_found != NULL) {
        config->handlers.device_found(config->handlers.context, &identity, dspi_link_baud(link));
    }
}

static void check_still_present(void)
{
    if (dspi_link_failures_in_a_row(&uart_task_state.link) < DSPI_UART_FAILURES_BEFORE_LOST) {
        return;
    }
    ESP_LOGW(TAG, "The DSPi stopped answering");
    report_device_lost();
    publish_status();
}

/* With the device present: one queued request, or a short listen for
 * notifications and a heartbeat when idle. */
static void serve_device(void)
{
    dspi_link_t *link = &uart_task_state.link;
    queued_request_t item;

    if (xQueueReceive(uart_task_state.queue, &item, pdMS_TO_TICKS(DSPI_UART_LISTEN_SLICE_MS)) ==
        pdTRUE) {
        if (item.reconfigure) {
            apply_reconfigure(&item.settings);
            return;
        }
        const dspi_reply_t reply = dspi_link_transact(link, &item.request, item.may_retry);
        deliver_reply(&item, &reply);
    } else if (dspi_link_idle_ms(link) >= DSPI_UART_HEARTBEAT_IDLE_MS) {
        /* A missed heartbeat shows up in check_still_present() below. */
        if (dspi_link_heartbeat(link, uart_task_state.identity.serial) ==
            DSPI_HEARTBEAT_OTHER_DEVICE) {
            ESP_LOGW(TAG, "Another DSPi has replaced %s", uart_task_state.identity.serial);
            report_device_lost(); /* The next pass finds the new one */
            publish_status();
            return;
        }
    } else {
        dspi_link_listen(link, DSPI_UART_IDLE_LISTEN_MS);
    }
    check_still_present();
}

static void uart_task(void *argument)
{
    (void)argument;
    for (;;) {
        uart_task_state.last_loop_ms = now_ms();
        if (uart_task_state.device_present) {
            serve_device();
        } else {
            look_for_device();
            if (!uart_task_state.device_present) {
                refuse_requests(DSPI_UART_PROBE_INTERVAL_MS);
            }
        }
    }
}

esp_err_t dspi_uart_start(const dspi_uart_config_t *config)
{
    if (uart_task_state.queue != NULL) {
        return ESP_ERR_INVALID_STATE; /* Already started */
    }
    uart_task_state.config = *config;

    esp_err_t err = reinstall_uart(config->tx_pin, config->rx_pin, config->baud);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART setup on TX %u, RX %u failed: %s", config->tx_pin, config->rx_pin,
                 esp_err_to_name(err));
        return err;
    }

    const dspi_port_t port = {
        .context = NULL,
        .write = port_write,
        .read = port_read,
        .set_baud = port_set_baud,
        .restart = port_restart,
        .now_ms = port_now_ms,
    };
    dspi_link_init(&uart_task_state.link, &port, config->baud, receive_buffer,
                   sizeof(receive_buffer), relay_notification, NULL);
    uart_task_state.last_loop_ms = now_ms();
    publish_status();

    /* Created last, so no other task sees a queue before the link is ready. */
    uart_task_state.queue = xQueueCreate(DSPI_UART_QUEUE_DEPTH, sizeof(queued_request_t));
    if (uart_task_state.queue == NULL) {
        err = ESP_ERR_NO_MEM;
    } else if (xTaskCreate(uart_task, "dspi_uart", DSPI_UART_TASK_STACK_BYTES, NULL,
                           DSPI_UART_TASK_PRIORITY, NULL) != pdPASS) {
        vQueueDelete(uart_task_state.queue);
        uart_task_state.queue = NULL;
        err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Starting the uart task failed: %s", esp_err_to_name(err));
        (void)uart_driver_delete(UART_PORT);
        uart_task_state.driver_installed = false;
        return err;
    }
    ESP_LOGI(TAG, "Looking for a DSPi on TX GPIO %u, RX GPIO %u", config->tx_pin, config->rx_pin);
    return ESP_OK;
}

esp_err_t dspi_uart_reconfigure(uint8_t tx_pin, uint8_t rx_pin, uint32_t baud, bool detect_baud)
{
    if (uart_task_state.queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const queued_request_t item = {
        .reconfigure = true,
        .settings = {.tx_pin = tx_pin, .rx_pin = rx_pin, .baud = baud, .detect_baud = detect_baud},
    };
    if (xQueueSend(uart_task_state.queue, &item, pdMS_TO_TICKS(DSPI_UART_RECONFIGURE_WAIT_MS)) !=
        pdTRUE) {
        ESP_LOGW(TAG, "The uart queue stayed full; link settings not applied");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t dspi_uart_submit(const dspi_request_t *request, bool may_retry, uint32_t tag)
{
    if (uart_task_state.queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const queued_request_t item = {.request = *request, .may_retry = may_retry, .tag = tag};
    return xQueueSend(uart_task_state.queue, &item, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

void dspi_uart_get_status(dspi_uart_status_t *status)
{
    portENTER_CRITICAL(&status_lock);
    *status = uart_task_state.published;
    portEXIT_CRITICAL(&status_lock);

    /* The counters change on every request, too often to publish. Each is
     * one aligned 32-bit word that only the uart task writes, so a copy may
     * mix two moments but never holds a torn value. */
    status->counters = *dspi_link_counters(&uart_task_state.link);
}

bool dspi_uart_is_alive(void)
{
    /* One pass of the loop can take a full search of every baud rate.
     * Unsigned subtraction stays right when the millisecond clock wraps. */
    if (uart_task_state.queue == NULL) {
        return false; /* Never started */
    }
    /* The loop time first, then the clock: read the other way round, the
     * uart task could record a time after the clock was read, and the
     * difference would wrap to a huge age. Signed, for the same reason. */
    const uint32_t last_loop_ms = uart_task_state.last_loop_ms;
    const int32_t age_ms = (int32_t)(now_ms() - last_loop_ms);
    return age_ms < DSPI_UART_ALIVE_WITHIN_MS;
}
