/*
 * status_led.h - drives the optional status LED (bridge spec 13).
 *
 * A timer redraws the LED every LED_FRAME_MS from led_pattern.h. It asks
 * `read_inputs` (supplied by the caller) for the network and DSPi state,
 * and takes the button's stage from status_led_show_button().
 *
 * The LED is a plain LED on a pin (driven through LEDC, so it can pulse),
 * on when the pin is high or when it is low, or one WS2812 addressable LED.
 * Every function may be called from any task.
 */

#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdint.h>

#include "button_machine.h"
#include "esp_err.h"
#include "led_pattern.h"
#include "settings_model.h"

/* Fills in the network and DSPi fields of `inputs`; the button field is
 * filled in here. Called on ESP-IDF's timer task. */
typedef void (*status_led_read_inputs_t)(led_inputs_t *inputs);

/* Starts drawing on the LED `type` at `pin`; SETTINGS_LED_NONE draws
 * nothing until status_led_configure() names an LED. */
esp_err_t status_led_start(settings_led_type_t type, uint8_t pin,
                           status_led_read_inputs_t read_inputs);

/* Moves to another LED, from a settings change (bridge spec 8.11). The old
 * pin is released. */
esp_err_t status_led_configure(settings_led_type_t type, uint8_t pin);

/* What a release of the BOOT button would do now. */
void status_led_show_button(button_stage_t stage);

#endif /* STATUS_LED_H */
