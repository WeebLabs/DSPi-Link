/*
 * settings.c - stores the bridge's saved state in flash (NVS).
 *
 * Implements bridge spec 11: the `bridge` namespace (one named key per
 * field of bridge_settings_t), `devices` (names by serial), `wifi` (the
 * home network) and `auth` (the PIN and paired clients, as one blob), plus
 * the factory reset of spec 12.1. The rules for what is valid live in
 * settings_model.c; this file only moves values between flash and memory.
 */

#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "bootloader_random.h"
#include "bridge_config.h"
#include "dspi_link.h"
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
 * value missing from flash keeps its default, and set `*needs_save` so the
 * caller writes that default back: otherwise a hub id lost to a power cut
 * would be made afresh on every boot. Any other error is real. */

static esp_err_t read_u8(nvs_handle_t nvs, const char *key, uint8_t *value, bool *needs_save)
{
    const esp_err_t err = nvs_get_u8(nvs, key, value);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *needs_save = true;
        return ESP_OK;
    }
    return err;
}

static esp_err_t read_u32(nvs_handle_t nvs, const char *key, uint32_t *value, bool *needs_save)
{
    const esp_err_t err = nvs_get_u32(nvs, key, value);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *needs_save = true;
        return ESP_OK;
    }
    return err;
}

/* The longest string in bridge_settings_t, which read_string's buffer holds. */
_Static_assert(SETTINGS_NAME_MAX_BYTES >= SETTINGS_HOSTNAME_MAX_CHARS &&
                   SETTINGS_NAME_MAX_BYTES >= SETTINGS_HUB_ID_CHARS,
               "read_string's buffer must fit every settings string");

static esp_err_t read_string(nvs_handle_t nvs, const char *key, char *value, size_t size,
                             bool *needs_save)
{
    char buffer[SETTINGS_NAME_MAX_BYTES + 1];
    size_t length = sizeof(buffer);
    const esp_err_t err = nvs_get_str(nvs, key, buffer, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *needs_save = true;
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH || (err == ESP_OK && length > size)) {
        /* Too long to be valid: keep the default, as for a missing key. */
        ESP_LOGW(TAG, "Saved %s is too long; using the default", key);
        *needs_save = true;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    memcpy(value, buffer, length);
    return ESP_OK;
}

/* Overlays every saved key onto `settings`. Stops at the first real error. */
static esp_err_t read_all(nvs_handle_t nvs, bridge_settings_t *settings, bool *needs_save)
{
    ESP_RETURN_ON_ERROR(
        read_string(nvs, KEY_HUB_ID, settings->hub_id, sizeof(settings->hub_id), needs_save), TAG,
        "reading %s", KEY_HUB_ID);
    ESP_RETURN_ON_ERROR(
        read_string(nvs, KEY_NAME, settings->name, sizeof(settings->name), needs_save), TAG,
        "reading %s", KEY_NAME);
    ESP_RETURN_ON_ERROR(
        read_string(nvs, KEY_HOSTNAME, settings->hostname, sizeof(settings->hostname), needs_save),
        TAG, "reading %s", KEY_HOSTNAME);
    ESP_RETURN_ON_ERROR(read_u8(nvs, KEY_UART_TX, &settings->uart_tx_pin, needs_save), TAG,
                        "reading %s", KEY_UART_TX);
    ESP_RETURN_ON_ERROR(read_u8(nvs, KEY_UART_RX, &settings->uart_rx_pin, needs_save), TAG,
                        "reading %s", KEY_UART_RX);
    ESP_RETURN_ON_ERROR(read_u32(nvs, KEY_UART_BAUD, &settings->uart_baud, needs_save), TAG,
                        "reading %s", KEY_UART_BAUD);
    ESP_RETURN_ON_ERROR(read_u8(nvs, KEY_LED_PIN, &settings->led_pin, needs_save), TAG,
                        "reading %s", KEY_LED_PIN);

    /* Stored as u8; converted here so the struct keeps its real types. */
    uint8_t detect = settings->uart_detect_baud ? 1 : 0;
    ESP_RETURN_ON_ERROR(read_u8(nvs, KEY_UART_DETECT, &detect, needs_save), TAG, "reading %s",
                        KEY_UART_DETECT);
    settings->uart_detect_baud = detect != 0;

    uint8_t led_type = (uint8_t)settings->led_type;
    ESP_RETURN_ON_ERROR(read_u8(nvs, KEY_LED_TYPE, &led_type, needs_save), TAG, "reading %s",
                        KEY_LED_TYPE);
    settings->led_type = (settings_led_type_t)led_type;

    return ESP_OK;
}

/* Writes every key, then the schema version, then commits. NVS writes each
 * key as it is set, so the schema goes last: a power cut part way leaves no
 * schema, and the next boot starts again from factory settings rather than
 * reading a half-written set. */
static esp_err_t write_all(nvs_handle_t nvs, const bridge_settings_t *settings)
{
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
    ESP_RETURN_ON_ERROR(nvs_set_u8(nvs, KEY_SCHEMA, SETTINGS_SCHEMA_VERSION), TAG, "writing %s",
                        KEY_SCHEMA);
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
    bool hub_id_missing = false; /* Everything is written below anyway */
    esp_err_t err = read_string(nvs, KEY_HUB_ID, hub_id, sizeof(hub_id), &hub_id_missing);
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

/* Reads saved settings of the current schema over `defaults`, repairs
 * what is invalid, and writes back whatever was missing or repaired. */
static esp_err_t load_current(nvs_handle_t nvs, bridge_settings_t *out,
                              const bridge_settings_t *defaults)
{
    /* Schema 1 is the only layout so far, so there is nothing to migrate
     * yet. A future schema adds its step here, oldest first. */
    *out = *defaults;
    bool needs_save = false;
    esp_err_t err = read_all(nvs, out, &needs_save);
    if (err != ESP_OK) {
        return err;
    }

    size_t repaired = 0;
    if (!settings_model_repair(out, defaults, board_table(), &repaired)) {
        ESP_LOGE(TAG, "Settings still invalid after repair (%s): not saving them",
                 settings_model_find_problem(out, board_table()));
        return ESP_ERR_INVALID_STATE;
    }
    if (repaired > 0) {
        ESP_LOGW(TAG, "%u saved value(s) were invalid on this board and were reset",
                 (unsigned)repaired);
    } else if (needs_save) {
        ESP_LOGW(TAG, "Some settings were missing from flash; saving their defaults");
    }
    return repaired > 0 || needs_save ? write_all(nvs, out) : ESP_OK;
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
        err = load_current(nvs, out, &defaults);
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

/* ---------------------------------------------------------------------------
 * Device names (namespace `devices`)
 * ------------------------------------------------------------------------- */

#define DEVICES_NAMESPACE "devices"
#define KEY_DEVICE_NAMES "names"

/* One saved name. The list is kept most recently saved first. */
typedef struct {
    char serial[DSPI_SERIAL_MAX_CHARS + 1];
    char name[SETTINGS_NAME_MAX_BYTES + 1];
} saved_device_name_t;

/* Reads the whole list into `names` (LINK_MAX_DEVICE_NAMES entries). A
 * missing list is an empty one, and so is one this firmware cannot read: a
 * blob of another size comes from a firmware with a different layout or
 * limit, and treating it as empty lets the next save replace it. */
static esp_err_t read_device_names(nvs_handle_t nvs, saved_device_name_t *names, size_t *count)
{
    *count = 0;
    size_t bytes = sizeof(saved_device_name_t) * LINK_MAX_DEVICE_NAMES;
    const esp_err_t err = nvs_get_blob(nvs, KEY_DEVICE_NAMES, names, &bytes);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH ||
        (err == ESP_OK && bytes % sizeof(saved_device_name_t) != 0)) {
        ESP_LOGW(TAG, "Saved device names have another layout; starting an empty list");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    *count = bytes / sizeof(saved_device_name_t);
    for (size_t i = 0; i < *count; i++) {
        /* Flash is not trusted to hold terminated strings. */
        names[i].serial[sizeof(names[i].serial) - 1] = '\0';
        names[i].name[sizeof(names[i].name) - 1] = '\0';
    }
    return ESP_OK;
}

esp_err_t settings_load_device_name(const char *serial, char *name, size_t size)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(DEVICES_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND; /* Nothing has ever been saved */
    }
    if (err != ESP_OK) {
        return err;
    }

    saved_device_name_t names[LINK_MAX_DEVICE_NAMES];
    size_t count = 0;
    err = read_device_names(nvs, names, &count);
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < count; i++) {
        if (strncmp(names[i].serial, serial, sizeof(names[i].serial)) == 0) {
            snprintf(name, size, "%s", names[i].name);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t settings_save_device_name(const char *serial, const char *name)
{
    if (!settings_name_is_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(DEVICES_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    saved_device_name_t names[LINK_MAX_DEVICE_NAMES];
    size_t count = 0;
    err = read_device_names(nvs, names, &count);
    if (err == ESP_OK) {
        /* Put this device first; drop its old entry, and the last entry if
         * the list is full. */
        saved_device_name_t updated[LINK_MAX_DEVICE_NAMES];
        memset(updated, 0, sizeof(updated));
        snprintf(updated[0].serial, sizeof(updated[0].serial), "%s", serial);
        snprintf(updated[0].name, sizeof(updated[0].name), "%s", name);
        size_t kept = 1;
        for (size_t i = 0; i < count && kept < LINK_MAX_DEVICE_NAMES; i++) {
            if (strncmp(names[i].serial, serial, sizeof(names[i].serial)) != 0) {
                updated[kept++] = names[i];
            }
        }
        err = nvs_set_blob(nvs, KEY_DEVICE_NAMES, updated, kept * sizeof(saved_device_name_t));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

/* ---------------------------------------------------------------------------
 * The home network (namespace `wifi`)
 * ------------------------------------------------------------------------- */

#define WIFI_NAMESPACE "wifi"
#define KEY_WIFI_SSID "ssid"
#define KEY_WIFI_PASSWORD "password"

esp_err_t settings_load_wifi(char ssid[SETTINGS_WIFI_SSID_MAX_BYTES + 1],
                             char password[SETTINGS_WIFI_PASSWORD_MAX_BYTES + 1])
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }
    size_t length = SETTINGS_WIFI_SSID_MAX_BYTES + 1;
    err = nvs_get_str(nvs, KEY_WIFI_SSID, ssid, &length);
    if (err == ESP_OK) {
        length = SETTINGS_WIFI_PASSWORD_MAX_BYTES + 1;
        err = nvs_get_str(nvs, KEY_WIFI_PASSWORD, password, &length);
    }
    nvs_close(nvs);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

esp_err_t settings_save_wifi(const char *ssid, const char *password)
{
    if (strlen(ssid) == 0 || strlen(ssid) > SETTINGS_WIFI_SSID_MAX_BYTES ||
        strlen(password) > SETTINGS_WIFI_PASSWORD_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs), TAG, "opening %s",
                        WIFI_NAMESPACE);
    esp_err_t err = nvs_set_str(nvs, KEY_WIFI_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, KEY_WIFI_PASSWORD, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t settings_forget_wifi(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(nvs);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

/* ---------------------------------------------------------------------------
 * The PIN and paired clients (namespace `auth`)
 * ------------------------------------------------------------------------- */

#define AUTH_NAMESPACE "auth"
#define KEY_AUTH_STORE "store"

esp_err_t settings_load_auth(void *blob, size_t size)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(AUTH_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }
    size_t length = 0;
    err = nvs_get_blob(nvs, KEY_AUTH_STORE, NULL, &length);
    if (err == ESP_OK && length != size) {
        ESP_LOGW(TAG, "Saved PIN data has another layout; starting without a PIN");
        err = ESP_ERR_NOT_FOUND;
    } else if (err == ESP_OK) {
        err = nvs_get_blob(nvs, KEY_AUTH_STORE, blob, &length);
    }
    nvs_close(nvs);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

esp_err_t settings_save_auth(const void *blob, size_t size)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(AUTH_NAMESPACE, NVS_READWRITE, &nvs), TAG, "opening %s",
                        AUTH_NAMESPACE);
    esp_err_t err = nvs_set_blob(nvs, KEY_AUTH_STORE, blob, size);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

/* ---------------------------------------------------------------------------
 * Factory reset
 * ------------------------------------------------------------------------- */

esp_err_t settings_erase_all(void)
{
    /* The NVS partition holds the bridge's namespaces and the radio's PHY
     * calibration (the Wi-Fi driver keeps no credentials there; see net.c).
     * Losing the calibration is harmless: the radio recalibrates at boot. */
    const esp_err_t err = nvs_flash_deinit();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGW(TAG, "Closing NVS before the erase failed: %s", esp_err_to_name(err));
    }
    return nvs_flash_erase();
}
