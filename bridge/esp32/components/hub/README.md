# hub

The DSPi Link hub (bridge spec 8): sessions, authentication, the device,
commands, notifications, polls, snapshots, locks, administration and the
hub's own firmware update. A plain state machine: one call per event, and
it answers through `hub_io_t`.

| File | What it does | Portable |
|------|--------------|----------|
| `include/hub.h`, `hub.c` | The hub's state, sessions, hello, the message dispatch table, keepalive, the info document and DNS-SD values. | yes |
| `hub_commands.c` | CMD frames, the queue of requests to the uart task, and which session caused a notification. | yes |
| `hub_device.c` | The device record and its events, notifications, snapshots, locks and device names. | yes |
| `hub_polls.c` | Poll subscriptions and their schedule. | yes |
| `hub_admin.c` | Administering the hub: settings, Wi-Fi, the PIN and paired clients, restart and reset (Link spec 7.10 to 7.13). | yes |
| `include/hub_config.h`, `hub_config.c` | The settings as `hub.config` JSON, and applying a change to them. | yes |
| `hub_update.c` | `hub.update` and its FWDATA chunks (Link spec 7.14). | yes |
| `hub_internal.h` | What the hub's files share; not public. | yes |
| `include/hub_io.h` | Everything the hub reaches outside itself, as functions it is given. | yes |
| `include/hub_task.h`, `hub_task.c` | The hub task: runs the hub on the ESP32, turns the HTTP server's, uart task's and ota task's reports into events, and supplies `hub_io_t`. | no |

Public API: `hub_init()`, `hub_tick()`, the `hub_session_*()`,
`hub_device_*()` and `hub_update_*()` events, `hub_rename()`,
`hub_restart()`, the PIN functions for the setup page, `hub_info_json()`
and `hub_advert()`. On the ESP32, `hub_task_start()` and the `hub_task_*()`
calls other tasks use.

Tests: `test/host/test_hub.c`, against `fake_hub_io.c` and `fake_dspi.c`.
