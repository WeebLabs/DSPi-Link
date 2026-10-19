/*
 * button_machine.h - what holding the BOOT button means (bridge spec 12.1).
 *
 * Feed it one sample of the pin at a time; it debounces, times the hold,
 * says which action a release would trigger right now (for the LED), and
 * reports the action itself on release:
 *
 *   held under 5 s     nothing
 *   held 5 to 15 s     forget Wi-Fi
 *   held 15 to 30 s    factory reset
 *   held over 30 s     nothing: the hold is cancelled
 *
 * The button counts only after it has been seen released once, so a pin
 * that reads pressed from the start (a board without the button, say)
 * never triggers anything.
 *
 * Portable: no ESP-IDF headers.
 */

#ifndef BUTTON_MACHINE_H
#define BUTTON_MACHINE_H

#include <stdbool.h>
#include <stdint.h>

/* What a release would do now. */
typedef enum {
    BUTTON_STAGE_NONE,          /* Not held, or held under 5 s */
    BUTTON_STAGE_FORGET_WIFI,   /* Held 5 to 15 s */
    BUTTON_STAGE_FACTORY_RESET, /* Held 15 to 30 s */
    BUTTON_STAGE_CANCELLED,     /* Held over 30 s */
} button_stage_t;

typedef enum {
    BUTTON_ACTION_NONE,
    BUTTON_ACTION_FORGET_WIFI,
    BUTTON_ACTION_FACTORY_RESET,
} button_action_t;

typedef struct {
    bool armed;          /* Seen released at least once */
    bool pressed;        /* The debounced state */
    bool last_sample;    /* The raw state of the previous sample */
    uint8_t same_count;  /* Raw samples in a row equal to last_sample */
    uint32_t pressed_ms; /* When the debounced press began */
    button_stage_t stage;
} button_machine_t;

void button_machine_init(button_machine_t *machine);

/* Feeds one sample: `down` is true while the button is pressed. Returns the
 * action to take now, which is BUTTON_ACTION_NONE except on a release. */
button_action_t button_machine_sample(button_machine_t *machine, bool down, uint32_t now_ms);

/* What a release would do now, for the LED. */
button_stage_t button_machine_stage(const button_machine_t *machine);

#endif /* BUTTON_MACHINE_H */
