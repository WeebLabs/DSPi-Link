/*
 * test_board_tables.c - checks every chip's pin table (bridge spec 4.4).
 *
 * The tables list the pins the bridge offers. These tests restate, from the
 * datasheets, the pins it must never offer, so a slip in either place shows
 * up as a disagreement between the two.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board_table.h"
#include "check.h"

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/* A chip's table plus the datasheet facts it must agree with. */
typedef struct {
    const board_table_t *table;
    const uint8_t *never_for_uart; /* Flash, PSRAM, USB, console, strapping */
    size_t never_for_uart_count;
    const uint8_t *input_only; /* May be RX, never TX or LED */
    size_t input_only_count;
} chip_facts_t;

static const uint8_t ESP32_NEVER[] = {0,  1,  2,  3,  5,  6,  7,  8,  9,  10, 11, 12,
                                      15, 16, 17, 20, 24, 28, 29, 30, 31, 37, 38};
static const uint8_t ESP32_INPUT_ONLY[] = {34, 35, 36, 39};
static const uint8_t ESP32S3_NEVER[] = {0,  3,  19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 30,
                                        31, 32, 33, 34, 35, 36, 37, 43, 44, 45, 46, 47, 48};
static const uint8_t ESP32C3_NEVER[] = {2, 8, 9, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21};
static const uint8_t ESP32C6_NEVER[] = {4,  5,  8,  9,  10, 11, 12, 13, 14, 15,
                                        16, 17, 24, 25, 26, 27, 28, 29, 30};

static const chip_facts_t CHIPS[] = {
    {&BOARD_TABLE_ESP32, ESP32_NEVER, COUNT(ESP32_NEVER), ESP32_INPUT_ONLY,
     COUNT(ESP32_INPUT_ONLY)},
    {&BOARD_TABLE_ESP32S3, ESP32S3_NEVER, COUNT(ESP32S3_NEVER), NULL, 0},
    {&BOARD_TABLE_ESP32C3, ESP32C3_NEVER, COUNT(ESP32C3_NEVER), NULL, 0},
    {&BOARD_TABLE_ESP32C6, ESP32C6_NEVER, COUNT(ESP32C6_NEVER), NULL, 0},
};

static bool is_sorted_and_unique(board_pin_list_t list)
{
    for (size_t i = 1; i < list.count; i++) {
        if (list.pins[i] <= list.pins[i - 1]) {
            return false;
        }
    }
    return list.count > 0;
}

static bool contains_any(board_pin_list_t list, const uint8_t *pins, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (board_pin_list_contains(list, pins[i])) {
            return true;
        }
    }
    return false;
}

static void lists_are_sorted_and_unique(void)
{
    for (size_t i = 0; i < COUNT(CHIPS); i++) {
        const board_table_t *table = CHIPS[i].table;
        CHECK(is_sorted_and_unique(table->uart_tx_pins));
        CHECK(is_sorted_and_unique(table->uart_rx_pins));
        CHECK(is_sorted_and_unique(table->led_pins));
    }
}

static void defaults_are_offered_and_distinct(void)
{
    for (size_t i = 0; i < COUNT(CHIPS); i++) {
        const board_table_t *table = CHIPS[i].table;
        CHECK(board_pin_list_contains(table->uart_tx_pins, table->default_uart_tx_pin));
        CHECK(board_pin_list_contains(table->uart_rx_pins, table->default_uart_rx_pin));
        CHECK(table->default_uart_tx_pin != table->default_uart_rx_pin);
    }
}

static void unsafe_pins_are_never_offered_for_the_uart(void)
{
    for (size_t i = 0; i < COUNT(CHIPS); i++) {
        const chip_facts_t *chip = &CHIPS[i];
        CHECK(!contains_any(chip->table->uart_tx_pins, chip->never_for_uart,
                            chip->never_for_uart_count));
        CHECK(!contains_any(chip->table->uart_rx_pins, chip->never_for_uart,
                            chip->never_for_uart_count));
    }
}

static void input_only_pins_are_never_outputs(void)
{
    for (size_t i = 0; i < COUNT(CHIPS); i++) {
        const chip_facts_t *chip = &CHIPS[i];
        CHECK(!contains_any(chip->table->uart_tx_pins, chip->input_only, chip->input_only_count));
        CHECK(!contains_any(chip->table->led_pins, chip->input_only, chip->input_only_count));
    }
}

static void boot_button_is_never_offered(void)
{
    for (size_t i = 0; i < COUNT(CHIPS); i++) {
        const board_table_t *table = CHIPS[i].table;
        CHECK(!board_pin_list_contains(table->uart_tx_pins, table->boot_button_pin));
        CHECK(!board_pin_list_contains(table->uart_rx_pins, table->boot_button_pin));
        CHECK(!board_pin_list_contains(table->led_pins, table->boot_button_pin));
    }
}

static void chip_names_match_esp_idf_targets(void)
{
    CHECK_STR_EQ(BOARD_TABLE_ESP32.chip_name, "esp32");
    CHECK_STR_EQ(BOARD_TABLE_ESP32S3.chip_name, "esp32s3");
    CHECK_STR_EQ(BOARD_TABLE_ESP32C3.chip_name, "esp32c3");
    CHECK_STR_EQ(BOARD_TABLE_ESP32C6.chip_name, "esp32c6");
}

void test_board_tables(void)
{
    RUN(lists_are_sorted_and_unique);
    RUN(defaults_are_offered_and_distinct);
    RUN(unsafe_pins_are_never_offered_for_the_uart);
    RUN(input_only_pins_are_never_outputs);
    RUN(boot_button_is_never_offered);
    RUN(chip_names_match_esp_idf_targets);
}
