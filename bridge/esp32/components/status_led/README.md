# status_led

The optional status LED (bridge spec 13): a plain LED on a pin, lit when
the pin is high or when it is low, or one WS2812 addressable LED.

| File | What it does | Portable |
|------|--------------|----------|
| `include/led_pattern.h`, `led_pattern.c` | Which colour to show at a given moment, from the network state, the DSPi and the BOOT button. | yes |
| `include/status_led.h`, `status_led.c` | Redraws the LED 50 times a second: a plain LED through LEDC, so it can pulse, or a WS2812 through `led_strip`. Moves to another LED when the settings change. | no |

Public API: `status_led_start()`, `status_led_configure()`,
`status_led_show_button()`; the pattern's `led_pattern_init()` and
`led_pattern_colour()`.

Tests: `test/host/test_button_led.c`.
