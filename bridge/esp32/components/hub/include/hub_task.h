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

#include "esp_err.h"
#include "settings_model.h"

/*
 * Starts the hub task, then the uart task and the HTTP servers that feed
 * it. Call once, after net_start(), so the first DNS-SD advertisement has
 * mDNS to go to. Keeps a copy of `settings` to save changes to.
 */
esp_err_t hub_task_start(const bridge_settings_t *settings);

#endif /* HUB_TASK_H */
