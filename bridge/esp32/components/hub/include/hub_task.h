/*
 * hub_task.h - the hub task: runs the hub on the ESP32.
 *
 * Implements bridge spec 6.2. The hub task owns the hub (hub.h). The HTTP
 * server's, uart and ota tasks never call the hub; they post events to the
 * hub task's queue, and the hub task handles one event at a time. This
 * file wires the hub's I/O (hub_io.h) to the real HTTP server, uart task,
 * flash and mDNS.
 */

#ifndef HUB_TASK_H
#define HUB_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "http_server.h"
#include "hub_auth.h"
#include "hub_io.h"
#include "settings_model.h"

/*
 * Starts the hub task, then the ota and uart tasks that feed it. Call once,
 * from app_main, after net_start(), so the first DNS-SD advertisement has
 * mDNS to go to. Keeps a copy of `settings` to save changes to. Returns
 * ESP_OK, ESP_ERR_NO_MEM, or the error of the timer, crypto, ota or uart
 * start that failed.
 */
esp_err_t hub_task_start(const bridge_settings_t *settings);

/* The handlers that connect the HTTP server's sessions to the hub task;
 * pass them to http_server_start(). Call from app_main. */
http_server_handlers_t hub_task_http_handlers(void);

/*
 * Functions other tasks (the setup page) use. Each runs on the hub task and
 * waits for it, so the hub's state stays the hub task's alone. Never call
 * them from the hub task itself.
 */

/* Renames the hub: saves and advertises the name. ESP_ERR_INVALID_ARG for a
 * name that is not 1 to 63 bytes of UTF-8, ESP_FAIL if it could not be
 * saved, ESP_ERR_TIMEOUT if the hub task was too busy to take the call. */
esp_err_t hub_task_rename(const char *name);

/* True when the PIN is on (bridge spec 9.2). Fails closed: if the hub task
 * cannot answer, it says true, so the caller asks for the PIN and
 * hub_task_check_pin() then refuses it. */
bool hub_task_pin_enabled(void);

/* Checks a PIN typed on the setup page from IPv4 `address`, under the
 * pairing rate limits. Returns HUB_AUTH_OK, HUB_AUTH_WRONG,
 * HUB_AUTH_RATE_LIMITED, or HUB_AUTH_UNAVAILABLE when the hub task could
 * not take the call or the hash failed: fails closed. */
hub_auth_result_t hub_task_check_pin(uint32_t address, const char *pin);

/* Turns the PIN on with `pin`, or off with NULL; every paired client is
 * forgotten (bridge spec 9.3). ESP_ERR_INVALID_ARG for a PIN that is not
 * six digits, ESP_FAIL if the hash could not be made (nothing changed),
 * ESP_ERR_TIMEOUT if the hub task was too busy to take the call. */
esp_err_t hub_task_set_pin(const char *pin);

/* Asks the hub task to tell every session the bridge is going, then
 * restart after erasing what `level` says, as hub.reset does. For the BOOT
 * button: it does not wait, so it is safe from a timer callback. Returns
 * ESP_OK once the request is queued, ESP_ERR_INVALID_STATE if the hub task
 * is not running or not alive, ESP_ERR_TIMEOUT if its queue is full. On
 * any error the caller must erase and restart by itself. */
esp_err_t hub_task_restart(hub_restart_level_t level);

/* True when the hub task's loop ran within HUB_ALIVE_WITHIN_MS. Safe from
 * any task; for the check that confirms a new firmware (bridge spec 14.2). */
bool hub_task_is_alive(void);

/* Copies the hub's current name into `name` (`size` bytes). Safe from any
 * task; an empty string if its lock could not be had in
 * HUB_INFO_LOCK_TIMEOUT_MS (logged). */
void hub_task_name(char *name, size_t size);

#endif /* HUB_TASK_H */
