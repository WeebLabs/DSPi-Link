/*
 * button.c - samples the BOOT button on a timer and feeds button_machine
 * (bridge spec 12.1). See button.h. The machine decides what a hold means;
 * this file only reads the pin and reports to the handlers.
 */

#include "button.h"

#include "bridge_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "button";

static struct {
    uint8_t pin;
    button_handlers_t handlers;
    button_machine_t machine;
    esp_timer_handle_t timer;
} button_state;

/* What the log says at each stage, so a board without an LED can still be
 * followed on the serial port (bridge spec 12.1). */
static const char *stage_description(button_stage_t stage)
{
    switch (stage) {
    case BUTTON_STAGE_FORGET_WIFI:
        return "release now to forget Wi-Fi";
    case BUTTON_STAGE_FACTORY_RESET:
        return "release now for a factory reset";
    case BUTTON_STAGE_CANCELLED:
        return "held too long: releasing now does nothing";
    case BUTTON_STAGE_NONE:
        break;
    }
    return "released";
}

static void sample(void *argument)
{
    (void)argument;
    const button_stage_t before = button_machine_stage(&button_state.machine);
    const bool down = gpio_get_level(button_state.pin) == 0;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const button_action_t action = button_machine_sample(&button_state.machine, down, now_ms);

    const button_stage_t after = button_machine_stage(&button_state.machine);
    if (after != before) {
        ESP_LOGI(TAG, "BOOT button: %s", stage_description(after));
        if (button_state.handlers.stage_changed != NULL) {
            button_state.handlers.stage_changed(button_state.handlers.context, after);
        }
    }
    if (action != BUTTON_ACTION_NONE) {
        ESP_LOGI(TAG, "BOOT button released after a long hold: %s",
                 action == BUTTON_ACTION_FACTORY_RESET ? "factory reset" : "forgetting Wi-Fi");
        if (button_state.handlers.action != NULL) {
            button_state.handlers.action(button_state.handlers.context, action);
        }
    }
}

esp_err_t button_start(uint8_t pin, const button_handlers_t *handlers)
{
    button_state.pin = pin;
    button_state.handlers = *handlers;
    button_machine_init(&button_state.machine);

    const gpio_config_t input = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, /* Boards have one too; this covers those that do not */
    };
    ESP_RETURN_ON_ERROR(gpio_config(&input), TAG, "configuring GPIO %u", pin);

    const esp_timer_create_args_t timer = {.callback = sample, .name = "button"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer, &button_state.timer), TAG, "creating the timer");
    return esp_timer_start_periodic(button_state.timer, (uint64_t)BUTTON_SAMPLE_MS * 1000);
}
