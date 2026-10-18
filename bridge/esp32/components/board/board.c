/*
 * board.c - picks the pin table for this chip and reports PSRAM.
 *
 * The ESP-IDF side of the board component; the tables themselves live in
 * the portable board_tables.c.
 */

#include "board.h"

#include "bridge_config.h"
#include "sdkconfig.h"

#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

const board_table_t *board_table(void)
{
#if CONFIG_IDF_TARGET_ESP32
    return &BOARD_TABLE_ESP32;
#elif CONFIG_IDF_TARGET_ESP32S3
    return &BOARD_TABLE_ESP32S3;
#elif CONFIG_IDF_TARGET_ESP32C3
    return &BOARD_TABLE_ESP32C3;
#elif CONFIG_IDF_TARGET_ESP32C6
    return &BOARD_TABLE_ESP32C6;
#else
#error "Unsupported chip: the bridge supports esp32, esp32s3, esp32c3 and esp32c6 (bridge spec 4.1)"
#endif
}

bool board_has_psram(void)
{
#if CONFIG_SPIRAM
    return esp_psram_is_initialized();
#else
    return false;
#endif
}

size_t board_max_sessions(void)
{
    return board_has_psram() ? LINK_MAX_SESSIONS_WITH_PSRAM : LINK_MAX_SESSIONS_WITHOUT_PSRAM;
}
