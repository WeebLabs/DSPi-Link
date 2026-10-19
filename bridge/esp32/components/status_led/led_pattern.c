/*
 * led_pattern.c - what the status LED shows, and when (bridge spec 13).
 * See led_pattern.h.
 */

#include "led_pattern.h"

#include "bridge_config.h"

static const led_colour_t OFF = {0, 0, 0};
static const led_colour_t BLUE = {0, 0, 255};
static const led_colour_t GREEN = {0, 255, 0};
static const led_colour_t AMBER = {255, 110, 0};
static const led_colour_t RED = {255, 0, 0};

/* On for the first half of every `period_ms`. */
static led_colour_t blink(led_colour_t colour, uint32_t period_ms, uint32_t now_ms)
{
    return now_ms % period_ms < period_ms / 2 ? colour : OFF;
}

/* Brightens and dims smoothly once every `period_ms`. */
static led_colour_t pulse(led_colour_t colour, uint32_t period_ms, uint32_t now_ms)
{
    const uint32_t half = period_ms / 2;
    const uint32_t phase = now_ms % period_ms;
    const uint32_t level = phase < half ? phase * 255 / half : (period_ms - phase) * 255 / half;
    return (led_colour_t){(uint8_t)(colour.red * level / 255),
                          (uint8_t)(colour.green * level / 255),
                          (uint8_t)(colour.blue * level / 255)};
}

void led_pattern_init(led_pattern_t *pattern)
{
    *pattern = (led_pattern_t){0};
}

led_colour_t led_pattern_colour(led_pattern_t *pattern, const led_inputs_t *inputs, uint32_t now_ms)
{
    const bool ready = inputs->network == LED_NETWORK_ONLINE && inputs->device_present;
    if (ready && !pattern->ready) {
        pattern->ready_since_ms = now_ms;
    }
    pattern->ready = ready;

    switch (inputs->button) {
    case BUTTON_STAGE_FORGET_WIFI:
        return blink(AMBER, LED_FAST_BLINK_MS, now_ms);
    case BUTTON_STAGE_FACTORY_RESET:
        return RED;
    case BUTTON_STAGE_CANCELLED:
        return OFF;
    case BUTTON_STAGE_NONE:
        break;
    }

    switch (inputs->network) {
    case LED_NETWORK_SETUP:
        return pulse(BLUE, LED_SLOW_PERIOD_MS, now_ms);
    case LED_NETWORK_CONNECTING:
        return blink(BLUE, LED_FAST_BLINK_MS, now_ms);
    case LED_NETWORK_ONLINE:
        break;
    }
    if (!inputs->device_present) {
        return blink(AMBER, LED_SLOW_PERIOD_MS, now_ms);
    }
    return now_ms - pattern->ready_since_ms < LED_READY_SHOW_MS ? GREEN : OFF;
}
