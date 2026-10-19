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
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "dspi_uart";

#define UART_PORT ((uart_port_t)DSPI_UART_PORT_NUMBER)

/* The heartbeat asks for the interface status, the cheapest read there is. */
#define REQ_GET_CTRL_IFACE_STATUS 0xF9

/* One queued item: a request for the DSPi, or new link settings. */
typedef struct {
    bool reconfigure; /* True: apply `settings`; false: send `request` */
    dspi_request_t request;
    bool may_retry;
    uint32_t tag;
    dspi_uart_config_t settings; /* Pins, rate and detection; handlers unused */
} queued_request_t;

/* Everything the uart task owns. Only the task touches it after start,
 * except `queue`, which FreeRTOS makes safe to send to from any task. */
static struct {
    dspi_uart_config_t config;
    dspi_link_t link;
    bool device_present;
    dspi_identity_t identity; /* Of the device present, for dspi_uart_get_status() */
    QueueHandle_t queue;
    int64_t last_loop_us; /* For dspi_uart_is_alive() */
} uart_task_state;

/* Holds one received payload, up to a whole bulk blob. Static so the task
 * stack stays small. */
static uint8_t receive_buffer[LINK_MAX_PAYLOAD_BYTES];

/* ---------------------------------------------------------------------------
 * The ESP32 UART as a dspi_port_t
 * ------------------------------------------------------------------------- */

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
    esp_err_t err = uart_driver_install(UART_PORT, DSPI_UART_DRIVER_RX_BUFFER_BYTES, 0, 0, NULL, 0);
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

static bool port_write(void *context, const uint8_t *data, size_t length)
{
    (void)context;
    if (uart_write_bytes(UART_PORT, data, length) != (int)length) {
        return false;
    }
    /* Generous: a 6 KB bulk payload takes 61 ms at 1 Mbaud, 530 ms at 115200. */
    return uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(1000)) == ESP_OK;
}

static size_t port_read(void *context, uint8_t *data, size_t capacity, uint32_t timeout_ms)
{
    (void)context;
    size_t buffered = 0;
    uart_get_buffered_data_len(UART_PORT, &buffered);

    if (buffered == 0) {
        /* uart_read_bytes waits for all the bytes asked for, so ask for one
         * to return as soon as anything arrives. */
        const int got = uart_read_bytes(UART_PORT, data, 1, pdMS_TO_TICKS(timeout_ms));
        if (got <= 0) {
            return 0;
        }
        uart_get_buffered_data_len(UART_PORT, &buffered);
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
    return uart_set_baudrate(UART_PORT, baud) == ESP_OK && uart_flush_input(UART_PORT) == ESP_OK;
}

static bool port_restart(void *context)
{
    (void)context;
    ESP_LOGW(TAG, "Reinstalling the UART driver after repeated timeouts");
    const dspi_uart_config_t *config = &uart_task_state.config;
    uart_driver_delete(UART_PORT);
    return install_uart(config->tx_pin, config->rx_pin, dspi_link_baud(&uart_task_state.link)) ==
           ESP_OK;
}

static uint32_t port_now_ms(void *context)
{
    (void)context;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---------------------------------------------------------------------------
 * The task
 * ------------------------------------------------------------------------- */

static void relay_notification(void *context, const uint8_t *packet, size_t length)
{
    (void)context;
    const dspi_uart_handlers_t *handlers = &uart_task_state.config.handlers;
    if (handlers->notification != NULL) {
        handlers->notification(handlers->context, packet, length);
    }
}

/* Moves the UART to new pins and rate. Whatever device was there is
 * reported lost; the task looks for it again on the new link. */
static void apply_reconfigure(const dspi_uart_config_t *settings)
{
    dspi_uart_config_t *config = &uart_task_state.config;
    const bool same_link = settings->tx_pin == config->tx_pin &&
                           settings->rx_pin == config->rx_pin &&
                           settings->baud == dspi_link_baud(&uart_task_state.link);
    if (same_link) {
        /* Already running like this: the usual case is the hub saving a rate
         * that detection has just found. Nothing to move, nothing lost. */
        config->baud = settings->baud;
        config->detect_baud = settings->detect_baud;
        return;
    }

    config->tx_pin = settings->tx_pin;
    config->rx_pin = settings->rx_pin;
    config->baud = settings->baud;
    config->detect_baud = settings->detect_baud;

    uart_driver_delete(UART_PORT);
    const esp_err_t err = install_uart(config->tx_pin, config->rx_pin, config->baud);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART setup on TX %u, RX %u failed: %s", config->tx_pin, config->rx_pin,
                 esp_err_to_name(err));
    }
    dspi_link_set_baud(&uart_task_state.link, config->baud);
    ESP_LOGI(TAG, "Link moved to TX GPIO %u, RX GPIO %u, %lu baud", config->tx_pin, config->rx_pin,
             (unsigned long)config->baud);

    if (uart_task_state.device_present) {
        uart_task_state.device_present = false;
        if (config->handlers.device_lost != NULL) {
            config->handlers.device_lost(config->handlers.context);
        }
    }
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

/* Probes for the DSPi once (bridge spec 7.5 and 7.6). */
static void look_for_device(void)
{
    static const uint32_t candidates[] = DSPI_UART_BAUD_CANDIDATES;
    const dspi_uart_config_t *config = &uart_task_state.config;
    dspi_link_t *link = &uart_task_state.link;

    const size_t candidate_count =
        config->detect_baud ? sizeof(candidates) / sizeof(candidates[0]) : 0;
    if (!dspi_link_find_baud(link, dspi_link_baud(link), candidates, candidate_count)) {
        return;
    }

    dspi_identity_t identity;
    if (!dspi_link_read_identity(link, &identity)) {
        ESP_LOGW(TAG, "A DSPi answered at %lu baud but its identity could not be read",
                 (unsigned long)dspi_link_baud(link));
        return;
    }

    uart_task_state.identity = identity;
    uart_task_state.device_present = true;
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
    uart_task_state.device_present = false;
    ESP_LOGW(TAG, "The DSPi stopped answering");
    const dspi_uart_handlers_t *handlers = &uart_task_state.config.handlers;
    if (handlers->device_lost != NULL) {
        handlers->device_lost(handlers->context);
    }
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
        const dspi_request_t heartbeat = {
            .is_get = true, .request = REQ_GET_CTRL_IFACE_STATUS, .length = 8};
        dspi_link_transact(link, &heartbeat, true);
    } else {
        dspi_link_listen(link, 1);
    }
    check_still_present();
}

static void uart_task(void *argument)
{
    (void)argument;
    for (;;) {
        uart_task_state.last_loop_us = esp_timer_get_time();
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
    uart_task_state.config = *config;
    uart_task_state.queue = xQueueCreate(DSPI_UART_QUEUE_DEPTH, sizeof(queued_request_t));
    if (uart_task_state.queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = install_uart(config->tx_pin, config->rx_pin, config->baud);
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

    if (xTaskCreate(uart_task, "dspi_uart", DSPI_UART_TASK_STACK_BYTES, NULL,
                    DSPI_UART_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Looking for a DSPi on TX GPIO %u, RX GPIO %u", config->tx_pin, config->rx_pin);
    return ESP_OK;
}

esp_err_t dspi_uart_reconfigure(uint8_t tx_pin, uint8_t rx_pin, uint32_t baud, bool detect_baud)
{
    const queued_request_t item = {
        .reconfigure = true,
        .settings = {.tx_pin = tx_pin, .rx_pin = rx_pin, .baud = baud, .detect_baud = detect_baud},
    };
    return xQueueSend(uart_task_state.queue, &item, pdMS_TO_TICKS(1000)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t dspi_uart_submit(const dspi_request_t *request, bool may_retry, uint32_t tag)
{
    const queued_request_t item = {.request = *request, .may_retry = may_retry, .tag = tag};
    return xQueueSend(uart_task_state.queue, &item, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

void dspi_uart_get_status(dspi_uart_status_t *status)
{
    status->tx_pin = uart_task_state.config.tx_pin;
    status->rx_pin = uart_task_state.config.rx_pin;
    status->baud = dspi_link_baud(&uart_task_state.link);
    status->device_present = uart_task_state.device_present;
    status->identity = uart_task_state.identity;
    status->counters = *dspi_link_counters(&uart_task_state.link);
}

bool dspi_uart_is_alive(void)
{
    /* One pass of the loop can take a full search of every baud rate. */
    return esp_timer_get_time() - uart_task_state.last_loop_us <
           (int64_t)DSPI_UART_ALIVE_WITHIN_MS * 1000;
}
