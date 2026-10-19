/*
 * button.h - watches the BOOT button (bridge spec 12.1).
 *
 * A timer samples the pin every BUTTON_SAMPLE_MS and feeds button_machine.h.
 * The handlers run on ESP-IDF's timer task: `stage_changed` whenever the
 * action a release would trigger changes (for the LED), and `action` on a
 * release that triggers one. Either may be NULL. They must return quickly
 * and never wait, since every other timer waits for them.
 */

#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>

#include "button_machine.h"
#include "esp_err.h"

typedef struct {
    void *context;
    void (*stage_changed)(void *context, button_stage_t stage);
    void (*action)(void *context, button_action_t action);
} button_handlers_t;

/* Starts watching the button on `pin`, which reads low while pressed, as
 * the BOOT button does on every common board. Called once, by app_main.
 * Returns the GPIO or timer error that stopped it. */
esp_err_t button_start(uint8_t pin, const button_handlers_t *handlers);

#endif /* BUTTON_H */
