/*
 * test_button_led.c - the BOOT button's hold timing (bridge spec 12.1) and
 * the status LED's patterns (bridge spec 13).
 */

#include "bridge_config.h"
#include "button_machine.h"
#include "check.h"
#include "led_pattern.h"

/* Holds the button (`down`) or leaves it for `ms`, one sample every 20 ms,
 * and returns the last action reported. */
static button_action_t hold(button_machine_t *machine, bool down, uint32_t *now_ms, uint32_t ms)
{
    button_action_t last = BUTTON_ACTION_NONE;
    for (uint32_t t = 0; t < ms; t += BUTTON_SAMPLE_MS) {
        *now_ms += BUTTON_SAMPLE_MS;
        const button_action_t action = button_machine_sample(machine, down, *now_ms);
        if (action != BUTTON_ACTION_NONE) {
            last = action;
        }
    }
    return last;
}

/* A fresh machine that has seen the button released. */
static void armed(button_machine_t *machine, uint32_t *now_ms)
{
    button_machine_init(machine);
    hold(machine, false, now_ms, 200);
}

static void a_short_press_does_nothing(void)
{
    button_machine_t machine;
    uint32_t now = 1000;
    armed(&machine, &now);
    CHECK(hold(&machine, true, &now, 3000) == BUTTON_ACTION_NONE);
    CHECK(button_machine_stage(&machine) == BUTTON_STAGE_NONE);
    CHECK(hold(&machine, false, &now, 200) == BUTTON_ACTION_NONE);
}

static void each_hold_length_triggers_its_action_on_release(void)
{
    button_machine_t machine;
    uint32_t now = 1000;
    armed(&machine, &now);
    hold(&machine, true, &now, 7000);
    CHECK(button_machine_stage(&machine) == BUTTON_STAGE_FORGET_WIFI);
    CHECK(hold(&machine, false, &now, 200) == BUTTON_ACTION_FORGET_WIFI);
    CHECK(button_machine_stage(&machine) == BUTTON_STAGE_NONE);

    hold(&machine, true, &now, 20000);
    CHECK(button_machine_stage(&machine) == BUTTON_STAGE_FACTORY_RESET);
    CHECK(hold(&machine, false, &now, 200) == BUTTON_ACTION_FACTORY_RESET);

    hold(&machine, true, &now, 31000);
    CHECK(button_machine_stage(&machine) == BUTTON_STAGE_CANCELLED);
    CHECK(hold(&machine, false, &now, 200) == BUTTON_ACTION_NONE); /* Changed their mind */
}

static void bounce_is_neither_a_press_nor_a_release(void)
{
    button_machine_t machine;
    uint32_t now = 1000;
    armed(&machine, &now);
    hold(&machine, true, &now, 6000);
    /* One stray reading mid-hold, as a bouncing contact gives. */
    CHECK(hold(&machine, false, &now, BUTTON_SAMPLE_MS) == BUTTON_ACTION_NONE);
    hold(&machine, true, &now, 2000);
    CHECK(button_machine_stage(&machine) == BUTTON_STAGE_FORGET_WIFI);
}

static void a_pin_pressed_from_the_start_never_acts(void)
{
    button_machine_t machine;
    button_machine_init(&machine);
    uint32_t now = 1000;
    hold(&machine, true, &now, 20000); /* No button: the pin reads low */
    CHECK(button_machine_stage(&machine) == BUTTON_STAGE_NONE);
    CHECK(hold(&machine, false, &now, 200) == BUTTON_ACTION_NONE);
}

static bool is(led_colour_t colour, uint8_t red, uint8_t green, uint8_t blue)
{
    return colour.red == red && colour.green == green && colour.blue == blue;
}

static void the_led_follows_the_network(void)
{
    led_pattern_t pattern;
    led_pattern_init(&pattern);
    led_inputs_t inputs = {.network = LED_NETWORK_CONNECTING};
    /* A fast blink: on, then off half a period later. */
    CHECK(is(led_pattern_colour(&pattern, &inputs, 1000), 0, 0, 255));
    CHECK(is(led_pattern_colour(&pattern, &inputs, 1000 + LED_FAST_BLINK_MS / 2), 0, 0, 0));

    /* A pulse: dark at the start of a period, full half way. */
    inputs.network = LED_NETWORK_SETUP;
    CHECK(is(led_pattern_colour(&pattern, &inputs, 4000), 0, 0, 0));
    CHECK(is(led_pattern_colour(&pattern, &inputs, 4000 + LED_SLOW_PERIOD_MS / 2), 0, 0, 255));

    inputs.network = LED_NETWORK_ONLINE;
    CHECK(led_pattern_colour(&pattern, &inputs, 6000).red == 255); /* Amber: no DSPi */

    inputs.device_present = true;
    CHECK(is(led_pattern_colour(&pattern, &inputs, 7000), 0, 255, 0));
    CHECK(is(led_pattern_colour(&pattern, &inputs, 7000 + LED_READY_SHOW_MS - 20), 0, 255, 0));
    CHECK(is(led_pattern_colour(&pattern, &inputs, 7000 + LED_READY_SHOW_MS), 0, 0, 0));
}

static void the_button_outranks_the_network(void)
{
    led_pattern_t pattern;
    led_pattern_init(&pattern);
    led_inputs_t inputs = {.network = LED_NETWORK_SETUP, .button = BUTTON_STAGE_FACTORY_RESET};
    CHECK(is(led_pattern_colour(&pattern, &inputs, 1000), 255, 0, 0));
    inputs.button = BUTTON_STAGE_CANCELLED;
    CHECK(is(led_pattern_colour(&pattern, &inputs, 1000 + LED_SLOW_PERIOD_MS / 2), 0, 0, 0));
    inputs.button = BUTTON_STAGE_FORGET_WIFI;
    CHECK(led_pattern_colour(&pattern, &inputs, 2000).red == 255);
}

void test_button_led(void)
{
    RUN(a_short_press_does_nothing);
    RUN(each_hold_length_triggers_its_action_on_release);
    RUN(bounce_is_neither_a_press_nor_a_release);
    RUN(a_pin_pressed_from_the_start_never_acts);
    RUN(the_led_follows_the_network);
    RUN(the_button_outranks_the_network);
}
