/*
 * settings.c - stores the bridge's settings in the NVS namespace `bridge`.
 *
 * Implements bridge spec 11. The rules for what is valid live in
 * settings_model.c; this file only moves values between flash and
 * bridge_settings_t, one named key per field.
 */

#include "settings.h"

#include <string.h>

#include "board.h"
#include "bootloader_random.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";

/* NVS namespace and key names (bridge spec 11). NVS limits names to 15
 * characters. */
#define NAMESPACE "bridge"
#define KEY_SCHEMA "schema"
#define KEY_HUB_ID "hid"
#define KEY_NAME "name"
#define KEY_HOSTNAME "hostname"
#define KEY_UART_TX "uart_tx"
#define KEY_UART_RX "uart_rx"
#define KEY_UART_BAUD "uart_baud"
#define KEY_UART_DETECT "uart_detect"
#define KEY_LED_TYPE "led_type"
#define KEY_LED_PIN "led_pin"

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS unusable (%s): erasing it, so all settings return to factory values",
                 esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_flash_init();
    }
    return err;
}

/* Builds the factory settings for this board. The hub id needs true
 * randomness, and before Wi-Fi starts the hardware random number generator
 * only has an entropy source while bootloader_random is enabled. */
static esp_err_t make_defaults(bridge_settings_t *out)
{
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t random[16];
    bootloader_random_enable();
    esp_fill_random(random, sizeof(random));
    bootloader_random_disable();

    settings_model_defaults(out, board_table(), mac, random);
    return ESP_OK;
}

/* The read helpers leave `value` untouched when the key is absent, so a
 * value missing from flash keeps its default. Any other error is real. */

static esp_err_t read_u8(nvs_handle_t nvs, const char *key, uint8_t *value)
{
    const esp_err_t err = nvs_get_u8(nvs, key, value);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static esp_err_t read_u32(nvs_handle_t nvs, const char *key, uint32_t *value)
{
    const esp_err_t err = nvs_get_u32(nvs, key, value);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static esp_err_t read_string(nvs_handle_t nvs, const char *key, char *value, size_t size)
{
    char buffer[SETTINGS_NAME_MAX_BYTES + 1];
    size_t length = sizeof(buffer);
    const esp_err_t err = nvs_get_str(nvs, key, buffer, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        /* Too long to be valid: keep the default, as for a missing key. */
        ESP_LOGW(TAG, "Saved %s is too long; using the default", key);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (length > size) {
        ESP_LOGW(TAG, "Saved %s is too long; using the default", key);
        return ESP_OK;
    }
    memcpy(value, buffer, length);
    return ESP_OK;
}

/* Overlays every saved key onto `settings`. Stops at the first real error. */
static esp_err_t read_all(nvs_handle_t nvs, bridge_settings_t *settings)
{
    ESP_RETURN_ON_ERROR(read_string(nvs, KEY_HUB_ID, settings->hub_id, sizeof(settings->hub_id)),
                        TAG, "reading %s", KEY_HUB_ID);
    ESP_RETURN_ON_ERROR(read_string(nvs, KEY_NAME, settings->name, sizeof(settings->name)), TAG,
                        "reading %s", KEY_NAME);
    ESP_RETURN_ON_ERROR(
        read_string(nvs, KEY_HOSTNAME, settings->hostname, sizeof(settings->hostname)), TAG,
        "reading %s", KEY_HOSTNAME);
    ESP_RETURN_ON_ERROR(read_u8(nvs, KEY_UART_TX, &settings->uart_tx_pin), TAG, "reading %s",
                        KEY_UART_TX);
    ESP_RETURN_ON_ERROR(read_u8(nvs, KEY_UART_RX, &settings->uart_rx_pin), TAG, "reading %s",
                        KEY_UART_RX);
    ESP_RETURN_ON_ERROR(read_u32(nvs, KEY_UART_BAUD, &settings->uart_baud), TAG, "reading %s",
                        KEY_UART_BAUD);
    ESP_RETURN_ON_ERROR(read_u8(nvs, KEY_LED_PIN, &settings->led_pin), TAG, "reading %s",
                        KEY_LED_PIN);

    /* Stored as u8; converted here so the struct keeps its real types. */
    uint8_t detect = settings->uart_detect_baud ? 1 : 0;
    ESP_RETURN_ON_ERROR(read_u8(nvs, KEY_UART_DETECT, &detect), TAG, "reading %s", KEY_UART_DETECT);
    settings->uart_detect_baud = detect != 0;

    uint8_t led_type = (uint8_t)settings->led_type;
    ESP_RETURN_ON_ERROR(read_u8(nvs, KEY_LED_TYPE, &led_type), TAG, "reading %s", KEY_LED_TYPE);
    settings->led_type = (settings_led_type_t)led_type;

    return ESP_OK;
}

/* Writes every key and the schema version, then commits once. */
static esp_err_t write_all(nvs_handle_t nvs, const bridge_settings_t *settings)
{
    ESP_RETURN_ON_ERROR(nvs_set_u8(nvs, KEY_SCHEMA, SETTINGS_SCHEMA_VERSION), TAG, "writing %s",
                        KEY_SCHEMA);
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, KEY_HUB_ID, settings->hub_id), TAG, "writing %s",
                        KEY_HUB_ID);
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, KEY_NAME, settings->name), TAG, "writing %s", KEY_NAME);
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, KEY_HOSTNAME, settings->hostname), TAG, "writing %s",
                        KEY_HOSTNAME);
    ESP_RETURN_ON_ERROR(nvs_set_u8(nvs, KEY_UART_TX, settings->uart_tx_pin), TAG, "writing %s",
                        KEY_UART_TX);
    ESP_RETURN_ON_ERROR(nvs_set_u8(nvs, KEY_UART_RX, settings->uart_rx_pin), TAG, "writing %s",
                        KEY_UART_RX);
    ESP_RETURN_ON_ERROR(nvs_set_u32(nvs, KEY_UART_BAUD, settings->uart_baud), TAG, "writing %s",
                        KEY_UART_BAUD);
    ESP_RETURN_ON_ERROR(nvs_set_u8(nvs, KEY_UART_DETECT, settings->uart_detect_baud ? 1 : 0), TAG,
                        "writing %s", KEY_UART_DETECT);
    ESP_RETURN_ON_ERROR(nvs_set_u8(nvs, KEY_LED_TYPE, (uint8_t)settings->led_type), TAG,
                        "writing %s", KEY_LED_TYPE);
    ESP_RETURN_ON_ERROR(nvs_set_u8(nvs, KEY_LED_PIN, settings->led_pin), TAG, "writing %s",
                        KEY_LED_PIN);
    return nvs_commit(nvs);
}

/* A newer firmware wrote these settings and this one was installed over it.
 * Bridge spec 11: reset the namespace to defaults, but keep the hub id so
 * paired clients still recognise the bridge. */
static esp_err_t reset_after_downgrade(nvs_handle_t nvs, bridge_settings_t *settings,
                                       const bridge_settings_t *defaults, uint8_t found_schema)
{
    ESP_LOGW(TAG, "Settings schema %u is newer than this firmware's %u: resetting to defaults",
             (unsigned)found_schema, (unsigned)SETTINGS_SCHEMA_VERSION);

    char hub_id[SETTINGS_HUB_ID_CHARS + 1];
    memcpy(hub_id, defaults->hub_id, sizeof(hub_id));
    esp_err_t err = read_string(nvs, KEY_HUB_ID, hub_id, sizeof(hub_id));
    if (err != ESP_OK) {
        return err;
    }

    *settings = *defaults;
    if (settings_hub_id_is_valid(hub_id)) {
        memcpy(settings->hub_id, hub_id, sizeof(settings->hub_id));
    }

    err = nvs_erase_all(nvs);
    return err == ESP_OK ? write_all(nvs, settings) : err;
}

esp_err_t settings_load(bridge_settings_t *out)
{
    bridge_settings_t defaults;
    esp_err_t err = make_defaults(&defaults);
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_open(NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t schema = 0;
    err = nvs_get_u8(nvs, KEY_SCHEMA, &schema);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "First boot: saving factory settings with hub id %s", defaults.hub_id);
        *out = defaults;
        err = write_all(nvs, out);
    } else if (err == ESP_OK && schema > SETTINGS_SCHEMA_VERSION) {
        err = reset_after_downgrade(nvs, out, &defaults, schema);
    } else if (err == ESP_OK) {
        /* Schema 1 is the only layout so far, so there is nothing to migrate
         * yet. A future schema adds its step here, oldest first. */
        *out = defaults;
        err = read_all(nvs, out);
        if (err == ESP_OK) {
            const size_t repaired = settings_model_repair(out, &defaults, board_table());
            if (repaired > 0) {
                ESP_LOGW(TAG, "%u saved value(s) were invalid on this board and were reset",
                         (unsigned)repaired);
                err = write_all(nvs, out);
            }
        }
    }

    nvs_close(nvs);
    return err;
}

esp_err_t settings_save(const bridge_settings_t *settings)
{
    const char *problem = settings_model_find_problem(settings, board_table());
    if (problem != NULL) {
        ESP_LOGE(TAG, "Refusing to save invalid settings: %s", problem);
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = write_all(nvs, settings);
    nvs_close(nvs);
    return err;
}
