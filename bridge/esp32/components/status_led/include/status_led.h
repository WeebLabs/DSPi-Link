/*
 * status_led.h - drives the optional status LED (bridge spec 13).
 *
 * A timer redraws the LED every LED_FRAME_MS from led_pattern.h. It asks
 * `read_inputs` (supplied by the caller) for the network and DSPi state,
 * and takes the button's stage from status_led_show_button().
 *
 * The LED is a plain LED on a pin (driven through LEDC, so it can pulse),
 * on when the pin is high or when it is low, or one WS2812 addressable LED.
 * Each function says which task may call it.
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
 * nothing until status_led_configure() names an LED. Called once, by
 * app_main. An LED that fails to start is logged, not an error; returns the
 * lock's or timer's error. */
esp_err_t status_led_start(settings_led_type_t type, uint8_t pin,
                           status_led_read_inputs_t read_inputs);

/* Moves to another LED, from a settings change (bridge spec 8.11). The old
 * pin is released. Any task but the timer task. Returns the new driver's
 * error, or ESP_ERR_TIMEOUT if a frame held the LED for longer than
 * LED_CONFIGURE_TIMEOUT_MS. */
esp_err_t status_led_configure(settings_led_type_t type, uint8_t pin);

/* What a release of the BOOT button would do now. Any task, the timer
 * task included: it only stores the stage. */
void status_led_show_button(button_stage_t stage);

#endif /* STATUS_LED_H */
