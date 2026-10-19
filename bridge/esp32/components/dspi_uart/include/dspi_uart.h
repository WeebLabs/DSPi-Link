/*
 * dspi_uart.h - the uart task: owns the UART wired to the DSPi.
 *
 * Implements bridge spec 6.2 (the uart task) and 7. The task finds the DSPi,
 * runs queued requests one at a time through dspi_link, relays
 * notifications, sends a heartbeat when the link is idle, and reports the
 * device found or lost.
 *
 * Every handler runs on the uart task. A handler must return quickly and
 * copy anything it keeps: pointers it receives are valid only during the
 * call. Call dspi_uart_start() once; dspi_uart_submit() may then be called
 * from any task, including from a handler.
 */

#ifndef DSPI_UART_H
#define DSPI_UART_H

#include <stdbool.h>
#include <stdint.h>

#include "dspi_link.h"
#include "esp_err.h"

/* What the uart task reports, and to whom. */
typedef struct {
    void *context; /* Passed first to every handler */

    /* A DSPi answered at `baud` and its identity was read. */
    void (*device_found)(void *context, const dspi_identity_t *identity, uint32_t baud);

    /* The DSPi stopped answering (bridge spec 7.6). Requests now get
     * DSPI_REPLY_NO_DEVICE until it is found again. */
    void (*device_lost)(void *context);

    /* One notification packet, verbatim. */
    void (*notification)(void *context, const uint8_t *packet, size_t length);

    /* The answer to a submitted request, with the caller's tag. */
    void (*reply)(void *context, uint32_t tag, const dspi_reply_t *reply);
} dspi_uart_handlers_t;

typedef struct {
    uint8_t tx_pin;   /* ESP32 pin wired to the DSPi's RX */
    uint8_t rx_pin;   /* ESP32 pin wired to the DSPi's TX */
    uint32_t baud;    /* The rate to try first */
    bool detect_baud; /* Also try DSPI_UART_BAUD_CANDIDATES (bridge spec 7.5) */
    dspi_uart_handlers_t handlers;
} dspi_uart_config_t;

/* Installs the UART driver and starts the uart task. Called once, from
 * app_main. Returns ESP_ERR_INVALID_STATE if already started, or the
 * driver's or FreeRTOS's error, having undone what it set up. */
esp_err_t dspi_uart_start(const dspi_uart_config_t *config);

/*
 * Queues a request; its answer arrives through `handlers.reply` with `tag`.
 * A SET's payload must stay valid until then. `may_retry` is as for
 * dspi_link_transact(). Returns ESP_ERR_NO_MEM if the queue is full and
 * ESP_ERR_INVALID_STATE before dspi_uart_start(); it never blocks. Any task
 * may call it, including a handler on the uart task.
 */
esp_err_t dspi_uart_submit(const dspi_request_t *request, bool may_retry, uint32_t tag);

/* Moves the link to new pins and a new rate, from hub.config.set (bridge
 * spec 8.11). The device is reported lost and then found again on the new
 * link, if it answers there. Queued behind any requests already waiting.
 * If the new pins cannot be set up, the uart task logs it and keeps the old
 * ones.
 *
 * Called by the hub task. It waits up to DSPI_UART_RECONFIGURE_WAIT_MS for
 * room in the queue and then returns ESP_ERR_TIMEOUT, so it must not be
 * called from a uart handler, which would wait on its own queue. Returns
 * ESP_ERR_INVALID_STATE before dspi_uart_start(). */
esp_err_t dspi_uart_reconfigure(uint8_t tx_pin, uint8_t rx_pin, uint32_t baud, bool detect_baud);

/* What hub.stats reports about the link (bridge spec 8.7). */
typedef struct {
    uint8_t tx_pin;
    uint8_t rx_pin;
    uint32_t baud;
    bool device_present;
    dspi_identity_t identity; /* Valid while device_present */
    dspi_link_counters_t counters;
} dspi_uart_status_t;

/* A copy of the link's status and counters, safe from any task. Pins, rate,
 * presence and identity are copied as one consistent snapshot; the counters
 * may be a moment apart from each other, which is fine for statistics. */
void dspi_uart_get_status(dspi_uart_status_t *status);

/* True when the uart task has gone round its loop within
 * DSPI_UART_ALIVE_WITHIN_MS; for the check that confirms a new firmware
 * (bridge spec 14.2). Safe from any task; false before dspi_uart_start(). */
bool dspi_uart_is_alive(void);

#endif /* DSPI_UART_H */
