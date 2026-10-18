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

#include <stddef.h>

#include "esp_err.h"
#include "http_server.h"
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

/* Renames the hub from another task (the setup page): posts the request to
 * the hub task, which saves and advertises the name. */
esp_err_t hub_task_rename(const char *name);

/* Copies the hub's current name. Safe from any task. */
void hub_task_name(char *name, size_t size);

#endif /* HUB_TASK_H */
