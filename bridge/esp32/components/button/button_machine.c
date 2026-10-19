/*
 * button_machine.c - what holding the BOOT button means (bridge spec 12.1).
 * See button_machine.h.
 */

#include "button_machine.h"

#include "bridge_config.h"

void button_machine_init(button_machine_t *machine)
{
    *machine = (button_machine_t){.stage = BUTTON_STAGE_NONE};
}

static button_stage_t stage_for_hold(uint32_t held_ms)
{
    if (held_ms >= BUTTON_CANCEL_HOLD_MS) {
        return BUTTON_STAGE_CANCELLED;
    }
    if (held_ms >= BUTTON_FACTORY_RESET_HOLD_MS) {
        return BUTTON_STAGE_FACTORY_RESET;
    }
    if (held_ms >= BUTTON_FORGET_WIFI_HOLD_MS) {
        return BUTTON_STAGE_FORGET_WIFI;
    }
    return BUTTON_STAGE_NONE;
}

button_action_t button_machine_sample(button_machine_t *machine, bool down, uint32_t now_ms)
{
    /* Debounce: the state changes only after the same raw reading several
     * samples in a row, so contact bounce is never a press or a release. */
    if (down == machine->last_sample) {
        if (machine->same_count < BUTTON_DEBOUNCE_SAMPLES) {
            machine->same_count++;
        }
    } else {
        machine->last_sample = down;
        machine->same_count = 1;
    }
    const bool settled = machine->same_count >= BUTTON_DEBOUNCE_SAMPLES;

    button_action_t action = BUTTON_ACTION_NONE;
    if (settled && down != machine->pressed) {
        machine->pressed = down;
        if (!down) {
            /* A release: act on the stage the LED was showing. */
            if (machine->armed && machine->stage == BUTTON_STAGE_FORGET_WIFI) {
                action = BUTTON_ACTION_FORGET_WIFI;
            } else if (machine->armed && machine->stage == BUTTON_STAGE_FACTORY_RESET) {
                action = BUTTON_ACTION_FACTORY_RESET;
            }
            machine->armed = true;
            machine->stage = BUTTON_STAGE_NONE;
        } else {
            machine->pressed_ms = now_ms;
        }
    }
    if (settled && !down) {
        machine->armed = true;
    }
    if (machine->pressed && machine->armed) {
        machine->stage = stage_for_hold(now_ms - machine->pressed_ms);
    }
    return action;
}

button_stage_t button_machine_stage(const button_machine_t *machine)
{
    return machine->stage;
}
