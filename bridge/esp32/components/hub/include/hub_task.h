/*
 * hub_task.h - the hub task: runs the hub on the ESP32.
 *
 * Implements bridge spec 6.2. The hub task owns the hub (hub.h). The HTTP
 * server's task and the uart task never call the hub; they post events to
 * the hub task's queue, and the hub task handles one event at a time. This
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
 * Starts the hub task, then the uart task that feeds it. Call once, after
 * net_start(), so the first DNS-SD advertisement has mDNS to go to. Keeps a
 * copy of `settings` to save changes to.
 */
esp_err_t hub_task_start(const bridge_settings_t *settings);

/* The handlers that connect the HTTP server's sessions to the hub task;
 * pass them to http_server_start(). */
http_server_handlers_t hub_task_http_handlers(void);

/*
 * Functions other tasks (the setup page) use. Each runs on the hub task and
 * waits for it, so the hub's state stays the hub task's alone.
 */

/* Renames the hub: saves and advertises the name. ESP_ERR_INVALID_ARG for a
 * name that is not 1 to 63 bytes of UTF-8. */
esp_err_t hub_task_rename(const char *name);

/* True when the PIN is on (bridge spec 9.2). */
bool hub_task_pin_enabled(void);

/* Checks a PIN typed on the setup page, under the pairing rate limits. */
hub_auth_result_t hub_task_check_pin(uint32_t address, const char *pin);

/* Turns the PIN on with `pin`, or off with NULL; every paired client is
 * forgotten (bridge spec 9.3). ESP_ERR_INVALID_ARG for a PIN that is not
 * six digits. */
esp_err_t hub_task_set_pin(const char *pin);

/* Tells every session the bridge is going, then restarts after erasing
 * what `level` says, as hub.reset does. For the BOOT button. */
esp_err_t hub_task_restart(hub_restart_level_t level);

/* True when the hub task's loop ran in the last 2 s. Safe from any task;
 * for the check that confirms a new firmware (bridge spec 14.2). */
bool hub_task_is_alive(void);

/* Copies the hub's current name. Safe from any task. */
void hub_task_name(char *name, size_t size);

#endif /* HUB_TASK_H */
