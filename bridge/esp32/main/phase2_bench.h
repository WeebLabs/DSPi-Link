/*
 * phase2_bench.h - TEMPORARY. Drives the uart task until the hub exists.
 *
 * Phase 2 of bridge/esp32/PLAN.md needs the UART link checked against a real
 * DSPi before the hub is written. This module starts the uart task and logs
 * everything it reports, so the serial monitor shows the device being found,
 * a bulk read, and each notification. Phase 3 deletes it and the hub takes
 * its place.
 */

#ifndef PHASE2_BENCH_H
#define PHASE2_BENCH_H

#include "esp_err.h"
#include "settings_model.h"

/* Starts the uart task with `settings` and logs what it reports. Keeps a
 * copy of the settings to save a newly detected baud rate. */
esp_err_t phase2_bench_start(const bridge_settings_t *settings);

#endif /* PHASE2_BENCH_H */
