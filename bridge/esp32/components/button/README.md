# button

The BOOT button's long presses (bridge spec 12.1): 5 to 15 s forgets Wi-Fi,
15 to 30 s is a factory reset, longer cancels.

| File | What it does | Portable |
|------|--------------|----------|
| `include/button_machine.h`, `button_machine.c` | Debounces samples of the pin, times the hold, says what a release would do now, and reports the action on release. | yes |
| `include/button.h`, `button.c` | Samples the pin every 20 ms on a timer and feeds the machine. | no |

Public API: `button_start()`; the machine's `button_machine_init()`,
`button_machine_sample()`, `button_machine_stage()`.

Tests: `test/host/test_button_led.c`.
