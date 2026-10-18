/*
 * hub_config.c - see hub_config.h; implements Link spec 7.10 for the keys of
 * bridge spec 8.11.
 *
 * Portable: no ESP-IDF headers.
 */

#include "hub_config.h"

#include <stdio.h>
#include <string.h>

#include "bridge_config.h"

/* The LED types by their names on the wire, in settings_led_type_t order. */
static const char *const LED_TYPE_NAMES[] = {"none", "gpio", "gpio_active_low", "ws2812"};
#define LED_TYPE_COUNT (sizeof(LED_TYPE_NAMES) / sizeof(LED_TYPE_NAMES[0]))

static cJSON *pin_list(board_pin_list_t list)
{
    cJSON *array = cJSON_CreateArray();
    for (size_t i = 0; i < list.count; i++) {
        cJSON_AddItemToArray(array, cJSON_CreateNumber(list.pins[i]));
    }
    return array;
}

cJSON *hub_config_to_json(const bridge_settings_t *settings)
{
    cJSON *config = cJSON_CreateObject();
    cJSON_AddStringToObject(config, "name", settings->name);
    cJSON_AddStringToObject(config, "hostname", settings->hostname);

    cJSON *uart = cJSON_AddObjectToObject(config, "uart");
    cJSON_AddNumberToObject(uart, "tx_pin", settings->uart_tx_pin);
    cJSON_AddNumberToObject(uart, "rx_pin", settings->uart_rx_pin);
    cJSON_AddNumberToObject(uart, "baud", settings->uart_baud);
    cJSON_AddBoolToObject(uart, "detect_baud", settings->uart_detect_baud);

    cJSON *led = cJSON_AddObjectToObject(config, "led");
    const unsigned type = (unsigned)settings->led_type;
    cJSON_AddStringToObject(led, "type", type < LED_TYPE_COUNT ? LED_TYPE_NAMES[type] : "none");
    if (settings->led_pin == BOARD_PIN_NONE) {
        cJSON_AddNullToObject(led, "pin");
    } else {
        cJSON_AddNumberToObject(led, "pin", settings->led_pin);
    }
    return config;
}

cJSON *hub_config_options(const board_table_t *board)
{
    static const uint32_t BAUDS[] = DSPI_UART_BAUD_CANDIDATES;
    cJSON *options = cJSON_CreateObject();

    cJSON *uart = cJSON_AddObjectToObject(options, "uart");
    cJSON_AddItemToObject(uart, "tx_pin", pin_list(board->uart_tx_pins));
    cJSON_AddItemToObject(uart, "rx_pin", pin_list(board->uart_rx_pins));
    cJSON *bauds = cJSON_AddArrayToObject(uart, "baud");
    for (size_t i = 0; i < sizeof(BAUDS) / sizeof(BAUDS[0]); i++) {
        cJSON_AddItemToArray(bauds, cJSON_CreateNumber(BAUDS[i]));
    }

    cJSON *led = cJSON_AddObjectToObject(options, "led");
    cJSON *types = cJSON_AddArrayToObject(led, "type");
    for (size_t i = 0; i < LED_TYPE_COUNT; i++) {
        cJSON_AddItemToArray(types, cJSON_CreateString(LED_TYPE_NAMES[i]));
    }
    cJSON_AddItemToObject(led, "pin", pin_list(board->led_pins));
    return options;
}

/* ---------------------------------------------------------------------------
 * Applying changes
 * ------------------------------------------------------------------------- */

/* Where a failed key's dotted path is written for the caller. Valid until
 * the next call; only the hub task calls this module. */
static char problem_path[48];

static const char *problem(const char *parent, const char *key)
{
    if (parent != NULL) {
        snprintf(problem_path, sizeof(problem_path), "%s.%s", parent, key);
    } else {
        snprintf(problem_path, sizeof(problem_path), "%s", key);
    }
    return problem_path;
}

/* A whole number from 0 to `max`, or false. */
static bool read_number(const cJSON *item, uint32_t max, uint32_t *out)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > max ||
        item->valuedouble != (double)(uint32_t)item->valuedouble) {
        return false;
    }
    *out = (uint32_t)item->valuedouble;
    return true;
}

/* A string that fits `size` bytes with its NUL, copied into `out`. */
static bool read_string(const cJSON *item, char *out, size_t size)
{
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= size) {
        return false;
    }
    snprintf(out, size, "%s", item->valuestring);
    return true;
}

static const char *apply_uart(const cJSON *uart, bridge_settings_t *result)
{
    if (!cJSON_IsObject(uart)) {
        return problem(NULL, "uart");
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, uart)
    {
        uint32_t number = 0;
        if (strcmp(item->string, "tx_pin") == 0 && read_number(item, 0xFE, &number)) {
            result->uart_tx_pin = (uint8_t)number;
        } else if (strcmp(item->string, "rx_pin") == 0 && read_number(item, 0xFE, &number)) {
            result->uart_rx_pin = (uint8_t)number;
        } else if (strcmp(item->string, "baud") == 0 && read_number(item, 10000000, &number)) {
            result->uart_baud = number;
        } else if (strcmp(item->string, "detect_baud") == 0 && cJSON_IsBool(item)) {
            result->uart_detect_baud = cJSON_IsTrue(item);
        } else {
            return problem("uart", item->string); /* Unknown, or the wrong type */
        }
    }
    return NULL;
}

static const char *apply_led(const cJSON *led, bridge_settings_t *result)
{
    if (!cJSON_IsObject(led)) {
        return problem(NULL, "led");
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, led)
    {
        uint32_t number = 0;
        if (strcmp(item->string, "type") == 0 && cJSON_IsString(item)) {
            size_t type = 0;
            while (type < LED_TYPE_COUNT && strcmp(item->valuestring, LED_TYPE_NAMES[type]) != 0) {
                type++;
            }
            if (type == LED_TYPE_COUNT) {
                return problem("led", "type");
            }
            result->led_type = (settings_led_type_t)type;
        } else if (strcmp(item->string, "pin") == 0 && cJSON_IsNull(item)) {
            result->led_pin = BOARD_PIN_NONE;
        } else if (strcmp(item->string, "pin") == 0 && read_number(item, 0xFE, &number)) {
            result->led_pin = (uint8_t)number;
        } else {
            return problem("led", item->string);
        }
    }
    /* Switching the LED off needs no pin; save the client from sending null. */
    if (result->led_type == SETTINGS_LED_NONE) {
        result->led_pin = BOARD_PIN_NONE;
    }
    return NULL;
}

const char *hub_config_apply(const cJSON *changes, const bridge_settings_t *current,
                             const board_table_t *board, bridge_settings_t *result)
{
    *result = *current;
    if (!cJSON_IsObject(changes)) {
        return problem(NULL, "config");
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, changes)
    {
        const char *failed = NULL;
        if (strcmp(item->string, "name") == 0) {
            failed = read_string(item, result->name, sizeof(result->name)) ? NULL
                                                                           : problem(NULL, "name");
        } else if (strcmp(item->string, "hostname") == 0) {
            failed = read_string(item, result->hostname, sizeof(result->hostname))
                         ? NULL
                         : problem(NULL, "hostname");
        } else if (strcmp(item->string, "uart") == 0) {
            failed = apply_uart(item, result);
        } else if (strcmp(item->string, "led") == 0) {
            failed = apply_led(item, result);
        } else {
            failed = problem(NULL, item->string); /* Unknown keys are errors (Link spec 7.10) */
        }
        if (failed != NULL) {
            return failed;
        }
    }
    return settings_model_find_problem(result, board);
}
