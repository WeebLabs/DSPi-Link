/*
 * led_pattern.h - what the status LED shows, and when (bridge spec 13).
 *
 * With the LED_* timings of bridge_config.h:
 *
 *   slow blue pulse           the setup network is up (setup or fallback)
 *   fast blue blink           joining the home network
 *   solid green for 5 s       online with the DSPi found, then off
 *   slow amber blink          online, but the DSPi is not answering
 *   fast amber blink          BOOT held 5 to 15 s: release to forget Wi-Fi
 *   solid red                 BOOT held 15 to 30 s: release to factory reset
 *   off                       BOOT held over 30 s: the hold is cancelled
 *
 * The button outranks the network, so the LED always says what a release
 * would do. Colours are at full scale; the driver dims them.
 *
 * Portable: no ESP-IDF headers.
 */

#ifndef LED_PATTERN_H
#define LED_PATTERN_H

#include <stdbool.h>
#include <stdint.h>

#include "button_machine.h"

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} led_colour_t;

typedef enum {
    LED_NETWORK_SETUP,      /* The setup network is up for a phone to join */
    LED_NETWORK_CONNECTING, /* Joining the home network */
    LED_NETWORK_ONLINE,     /* On the home network */
} led_network_t;

/* What the LED reports on. */
typedef struct {
    led_network_t network;
    bool device_present;
    button_stage_t button;
} led_inputs_t;

/* Remembers when the bridge became ready, for the 5 s of green. */
typedef struct {
    bool ready;
    uint32_t ready_since_ms;
} led_pattern_t;

/* Starts a pattern with the bridge not yet ready. */
void led_pattern_init(led_pattern_t *pattern);

/* The colour to show at `now_ms`. Call it often (every LED_FRAME_MS) so
 * blinks and pulses look smooth. */
led_colour_t led_pattern_colour(led_pattern_t *pattern, const led_inputs_t *inputs,
                                uint32_t now_ms);

#endif /* LED_PATTERN_H */
