/*
 * The bridge PIN's wording and rules, shared by the setup page and the
 * settings page so both say exactly the same thing (bridge spec 10.2 and
 * 10.3, Link spec 6.2).
 */

/** The label of the switch that turns the PIN on. */
export const PIN_SWITCH_LABEL = "Protect this bridge with a PIN";

/** What the switch protects, shown under it. */
export const PIN_SWITCH_HELP =
  "When this is on, anyone who opens these pages or connects from DSPi Console must enter " +
  "the PIN once on each device. When it's off, anyone on your network can control your DSPi " +
  "and change these settings.";

/** Shown wherever a PIN is chosen. */
export const PIN_REMINDER =
  "You'll need this PIN the first time you connect from each browser, computer or phone. " +
  "If you forget it, hold the BOOT button for 15 seconds to factory reset the bridge.";

/** A fixed PIN has exactly this many digits (Link spec 6.2). */
export const PIN_LENGTH = 6;

/**
 * @param pin What the user typed.
 * @returns True for exactly six digits, as a fixed PIN must be (Link spec 6.2 and 7.12).
 */
export function pinIsValid(pin: string): boolean {
  return new RegExp(`^[0-9]{${PIN_LENGTH}}$`).test(pin);
}

/**
 * Keeps only digits, at most six, so a PIN field cannot hold anything else.
 *
 * @param text A PIN field's value.
 * @returns The digits in it, cut to the PIN's length.
 */
export function digitsOnly(text: string): string {
  return text.replace(/[^0-9]/g, "").slice(0, PIN_LENGTH);
}
