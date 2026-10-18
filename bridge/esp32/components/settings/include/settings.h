/*
 * settings.h - loads and saves the bridge's settings in flash (NVS).
 *
 * Implements bridge spec 11 for the `bridge` and `devices` namespaces; the
 * `wifi` and `auth` namespaces join them in later phases. This is the only module
 * that touches NVS. Call settings_init() once at boot before anything else
 * uses flash storage. The load and save functions may be called from any
 * task, because NVS serialises access itself; in practice the hub task is
 * their only caller after boot.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include "esp_err.h"
#include "settings_model.h"

/*
 * Initialises NVS. If the partition is full or was written by an
 * incompatible NVS version, erases it and starts again, logging a warning:
 * the bridge then behaves as after a factory reset.
 */
esp_err_t settings_init(void);

/*
 * Loads the bridge's settings into `out`.
 *
 * On first boot this creates the factory settings, including a new hub id,
 * and saves them. A saved value that is invalid on this board is replaced
 * by its default and the repaired settings are saved, with a warning.
 */
esp_err_t settings_load(bridge_settings_t *out);

/* Saves `settings`, which must pass settings_model_find_problem(). */
esp_err_t settings_save(const bridge_settings_t *settings);

/*
 * Device names, saved by serial in the `devices` namespace (bridge spec
 * 8.7 and 11). Up to LINK_MAX_DEVICE_NAMES are kept; saving a new one when
 * full replaces the name used least recently.
 */

/* Copies the saved name for `serial` into `name`. ESP_ERR_NOT_FOUND if
 * there is none. */
esp_err_t settings_load_device_name(const char *serial, char *name, size_t size);

esp_err_t settings_save_device_name(const char *serial, const char *name);

#endif /* SETTINGS_H */
