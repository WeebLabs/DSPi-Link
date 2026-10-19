/*
 * test_settings_model.c - defaults and validation of the bridge's settings
 * (bridge spec 8.11 and 11).
 */

#include <stdint.h>
#include <string.h>

#include "board_table.h"
#include "check.h"
#include "settings_model.h"

static const uint8_t MAC[6] = {0xA0, 0xB7, 0x65, 0x12, 0x3F, 0x2A};
static const uint8_t RANDOM[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static bridge_settings_t defaults_for(const board_table_t *board)
{
    bridge_settings_t settings;
    settings_model_defaults(&settings, board, MAC, RANDOM);
    return settings;
}

static void defaults_are_valid_on_every_chip(void)
{
    const board_table_t *boards[] = {&BOARD_TABLE_ESP32, &BOARD_TABLE_ESP32S3, &BOARD_TABLE_ESP32C3,
                                     &BOARD_TABLE_ESP32C6};
    for (size_t i = 0; i < sizeof(boards) / sizeof(boards[0]); i++) {
        const bridge_settings_t settings = defaults_for(boards[i]);
        CHECK_NULL(settings_model_find_problem(&settings, boards[i]));
    }
}

static void defaults_are_named_after_the_mac(void)
{
    const bridge_settings_t settings = defaults_for(&BOARD_TABLE_ESP32S3);
    CHECK_STR_EQ(settings.name, "DSPi Bridge 3F2A");
    CHECK_STR_EQ(settings.hostname, "dspi-bridge-3f2a");
    CHECK(settings.uart_tx_pin == 17);
    CHECK(settings.uart_rx_pin == 18);
    CHECK(settings.uart_baud == 1000000);
    CHECK(settings.uart_detect_baud);
    CHECK(settings.led_type == SETTINGS_LED_NONE);
    CHECK(settings.led_pin == BOARD_PIN_NONE);
}

static void hub_id_is_a_version_4_uuid(void)
{
    const bridge_settings_t settings = defaults_for(&BOARD_TABLE_ESP32);
    /* All-ones input shows the version and variant bits being forced. */
    CHECK_STR_EQ(settings.hub_id, "ffffffff-ffff-4fff-bfff-ffffffffffff");
    CHECK(settings_hub_id_is_valid(settings.hub_id));
    CHECK(!settings_hub_id_is_valid("FFFFFFFF-FFFF-4FFF-BFFF-FFFFFFFFFFFF"));
    CHECK(!settings_hub_id_is_valid("ffffffff-ffff-4fff-bfff-fffffffffff"));
}

static void names_must_be_short_printable_utf8(void)
{
    char too_long[65];
    memset(too_long, 'a', 64);
    too_long[64] = '\0';

    CHECK(settings_name_is_valid("Living Room DSPi"));
    CHECK(settings_name_is_valid("Caf\xC3\xA9 DSPi"));
    CHECK(settings_name_is_valid(too_long + 1)); /* 63 bytes */
    CHECK(!settings_name_is_valid(too_long));    /* 64 bytes */
    CHECK(!settings_name_is_valid(""));
    CHECK(!settings_name_is_valid("Tab\there"));
    CHECK(!settings_name_is_valid("Cut short \xC3"));
    CHECK(!settings_name_is_valid("Stray \x80 byte"));
}

static void hostnames_must_be_dns_labels(void)
{
    CHECK(settings_hostname_is_valid("dspi-bridge-3f2a"));
    CHECK(settings_hostname_is_valid("studio2"));
    CHECK(!settings_hostname_is_valid(""));
    CHECK(!settings_hostname_is_valid("-studio"));
    CHECK(!settings_hostname_is_valid("studio-"));
    CHECK(!settings_hostname_is_valid("Studio"));
    CHECK(!settings_hostname_is_valid("studio.local"));
    CHECK(!settings_hostname_is_valid("studio_room"));
}

static void problems_are_reported_by_key(void)
{
    const board_table_t *board = &BOARD_TABLE_ESP32S3;
    bridge_settings_t settings;

    settings = defaults_for(board);
    settings.uart_tx_pin = 19; /* USB D- on the S3 */
    CHECK_STR_EQ(settings_model_find_problem(&settings, board), "uart.tx_pin");

    settings = defaults_for(board);
    settings.uart_rx_pin = settings.uart_tx_pin;
    CHECK_STR_EQ(settings_model_find_problem(&settings, board), "uart.rx_pin");

    settings = defaults_for(board);
    settings.uart_baud = 12345;
    CHECK_STR_EQ(settings_model_find_problem(&settings, board), "uart.baud");

    settings = defaults_for(board);
    settings.led_pin = 48; /* A pin without an LED type */
    CHECK_STR_EQ(settings_model_find_problem(&settings, board), "led.pin");

    settings = defaults_for(board);
    settings.led_type = SETTINGS_LED_WS2812;
    settings.led_pin = settings.uart_tx_pin;
    CHECK_STR_EQ(settings_model_find_problem(&settings, board), "led.pin");

    settings = defaults_for(board);
    settings.led_type = (settings_led_type_t)9;
    CHECK_STR_EQ(settings_model_find_problem(&settings, board), "led.type");

    settings = defaults_for(board);
    settings.led_type = SETTINGS_LED_WS2812;
    settings.led_pin = 48;
    CHECK_NULL(settings_model_find_problem(&settings, board));
}

static void repair_restores_defaults_for_each_bad_key(void)
{
    const board_table_t *board = &BOARD_TABLE_ESP32C3;
    const bridge_settings_t defaults = defaults_for(board);

    bridge_settings_t settings = defaults;
    settings.uart_tx_pin = 17; /* Flash on the C3 */
    settings.led_type = SETTINGS_LED_GPIO;
    settings.led_pin = 21; /* Console UART on the C3 */
    strcpy(settings.hostname, "Bad Name");

    size_t repaired = 0;
    CHECK(settings_model_repair(&settings, &defaults, board, &repaired));
    CHECK(repaired == 3);
    CHECK_NULL(settings_model_find_problem(&settings, board));
    CHECK(settings.uart_tx_pin == defaults.uart_tx_pin);
    CHECK(settings.led_type == SETTINGS_LED_NONE);
    CHECK_STR_EQ(settings.hostname, defaults.hostname);

    CHECK(settings_model_repair(&settings, &defaults, board, &repaired));
    CHECK(repaired == 0);
}

/* Defaults that are themselves invalid (a board table bug) must not pass
 * as repaired, or settings.c would save them. */
static void repair_fails_when_the_defaults_are_invalid(void)
{
    const board_table_t *board = &BOARD_TABLE_ESP32C3;
    bridge_settings_t defaults = defaults_for(board);
    defaults.uart_tx_pin = 17; /* Flash on the C3: never valid */

    bridge_settings_t settings = defaults;
    size_t repaired = 0;
    CHECK(!settings_model_repair(&settings, &defaults, board, &repaired));
    CHECK(repaired > 0);
    CHECK_STR_EQ(settings_model_find_problem(&settings, board), "uart.tx_pin");
}

void test_settings_model(void)
{
    RUN(defaults_are_valid_on_every_chip);
    RUN(defaults_are_named_after_the_mac);
    RUN(hub_id_is_a_version_4_uuid);
    RUN(names_must_be_short_printable_utf8);
    RUN(hostnames_must_be_dns_labels);
    RUN(problems_are_reported_by_key);
    RUN(repair_restores_defaults_for_each_bad_key);
    RUN(repair_fails_when_the_defaults_are_invalid);
}
