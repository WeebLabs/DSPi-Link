/*
 * status_led.c - drives the optional status LED. See status_led.h.
 */

#include "status_led.h"

#include <stdatomic.h>

#include "bridge_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"

static const char *TAG = "status_led";

/* A plain LED's brightness comes from LEDC: 8-bit duty at 5 kHz, far above
 * any visible flicker. */
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_CHANNEL LEDC_CHANNEL_0
#define LEDC_FREQUENCY_HZ 5000

/* The RMT clock for the WS2812's 1.25 µs bits. */
#define WS2812_RESOLUTION_HZ (10 * 1000 * 1000)

static struct {
    SemaphoreHandle_t lock; /* Held while the driver is changed or drawn */
    settings_led_type_t type;
    uint8_t pin;
    led_strip_handle_t strip; /* SETTINGS_LED_WS2812 only */
    led_pattern_t pattern;
    led_colour_t shown;
    bool shown_valid; /* False after a change, so the next frame is drawn */
    status_led_read_inputs_t read_inputs;
    atomic_int button; /* A button_stage_t */
    esp_timer_handle_t timer;
} led_state;

/* ---------------------------------------------------------------------------
 * Drivers. Called with the lock held.
 * ------------------------------------------------------------------------- */

static void stop_driver(void)
{
    switch (led_state.type) {
    case SETTINGS_LED_GPIO:
    case SETTINGS_LED_GPIO_ACTIVE_LOW:
        ledc_stop(LEDC_MODE, LEDC_CHANNEL, led_state.type == SETTINGS_LED_GPIO_ACTIVE_LOW);
        gpio_reset_pin(led_state.pin);
        break;
    case SETTINGS_LED_WS2812:
        led_strip_clear(led_state.strip);
        led_strip_del(led_state.strip);
        led_state.strip = NULL;
        gpio_reset_pin(led_state.pin);
        break;
    case SETTINGS_LED_NONE:
        break;
    }
    led_state.type = SETTINGS_LED_NONE;
}

static esp_err_t start_plain_led(uint8_t pin, bool active_low)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "LEDC timer");
    const ledc_channel_config_t channel = {
        .gpio_num = pin,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .flags.output_invert = active_low,
    };
    return ledc_channel_config(&channel);
}

static esp_err_t start_ws2812(uint8_t pin)
{
    const led_strip_config_t strip = {
        .strip_gpio_num = pin,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = WS2812_RESOLUTION_HZ,
    };
    return led_strip_new_rmt_device(&strip, &rmt, &led_state.strip);
}

static esp_err_t start_driver(settings_led_type_t type, uint8_t pin)
{
    esp_err_t err = ESP_OK;
    switch (type) {
    case SETTINGS_LED_GPIO:
    case SETTINGS_LED_GPIO_ACTIVE_LOW:
        err = start_plain_led(pin, type == SETTINGS_LED_GPIO_ACTIVE_LOW);
        break;
    case SETTINGS_LED_WS2812:
        err = start_ws2812(pin);
        break;
    case SETTINGS_LED_NONE:
        break;
    }
    if (err == ESP_OK) {
        led_state.type = type;
        led_state.pin = pin;
        led_state.shown_valid = false;
    } else {
        ESP_LOGW(TAG, "No status LED on GPIO %u: %s", pin, esp_err_to_name(err));
    }
    return err;
}

static void draw(led_colour_t colour)
{
    switch (led_state.type) {
    case SETTINGS_LED_GPIO:
    case SETTINGS_LED_GPIO_ACTIVE_LOW: {
        /* One colour: show the brightest component, so every pattern keeps
         * its timing (bridge spec 13). */
        uint8_t level = colour.red > colour.green ? colour.red : colour.green;
        level = colour.blue > level ? colour.blue : level;
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, level);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
        break;
    }
    case SETTINGS_LED_WS2812:
        led_strip_set_pixel(led_state.strip, 0, colour.red * LED_WS2812_BRIGHTNESS / 255,
                            colour.green * LED_WS2812_BRIGHTNESS / 255,
                            colour.blue * LED_WS2812_BRIGHTNESS / 255);
        led_strip_refresh(led_state.strip);
        break;
    case SETTINGS_LED_NONE:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * The frame timer
 * ------------------------------------------------------------------------- */

static void frame(void *argument)
{
    (void)argument;
    /* Never wait on the timer task: if the LED is being changed, skip a
     * frame. */
    if (xSemaphoreTake(led_state.lock, 0) != pdTRUE) {
        return;
    }
    if (led_state.type != SETTINGS_LED_NONE) {
        led_inputs_t inputs = {.network = LED_NETWORK_SETUP};
        led_state.read_inputs(&inputs);
        inputs.button = (button_stage_t)atomic_load(&led_state.button);
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        const led_colour_t colour = led_pattern_colour(&led_state.pattern, &inputs, now_ms);

        const bool changed = !led_state.shown_valid || colour.red != led_state.shown.red ||
                             colour.green != led_state.shown.green ||
                             colour.blue != led_state.shown.blue;
        if (changed) {
            draw(colour);
            led_state.shown = colour;
            led_state.shown_valid = true;
        }
    }
    xSemaphoreGive(led_state.lock);
}

/* ---------------------------------------------------------------------------
 * Public functions
 * ------------------------------------------------------------------------- */

esp_err_t status_led_start(settings_led_type_t type, uint8_t pin,
                           status_led_read_inputs_t read_inputs)
{
    led_state.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(led_state.lock != NULL, ESP_ERR_NO_MEM, TAG, "lock");
    led_state.read_inputs = read_inputs;
    led_pattern_init(&led_state.pattern);
    atomic_store(&led_state.button, BUTTON_STAGE_NONE);

    /* A missing or miswired LED is not worth failing the boot over. */
    start_driver(type, pin);

    const esp_timer_create_args_t timer = {.callback = frame, .name = "status_led"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer, &led_state.timer), TAG, "timer");
    return esp_timer_start_periodic(led_state.timer, (uint64_t)LED_FRAME_MS * 1000);
}

esp_err_t status_led_configure(settings_led_type_t type, uint8_t pin)
{
    xSemaphoreTake(led_state.lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (type != led_state.type || pin != led_state.pin) {
        stop_driver();
        err = start_driver(type, pin);
    }
    xSemaphoreGive(led_state.lock);
    return err;
}

void status_led_show_button(button_stage_t stage)
{
    atomic_store(&led_state.button, (int)stage);
}
