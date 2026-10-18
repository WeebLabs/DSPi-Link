/*
 * The bridge PIN's wording and rules, shared by the setup page and the
 * settings page so both say exactly the same thing (bridge spec 10.2, 10.3).
 */

export const PIN_SWITCH_LABEL = "Protect this bridge with a PIN";

export const PIN_SWITCH_HELP =
  "When this is on, anyone who opens these pages or connects from DSPi Console must enter " +
  "the PIN once on each device. When it's off, anyone on your network can control your DSPi " +
  "and change these settings.";

export const PIN_REMINDER =
  "You'll need this PIN the first time you connect from each browser, computer or phone. " +
  "If you forget it, hold the BOOT button for 15 seconds to factory reset the bridge.";

export const PIN_LENGTH = 6;

/** Exactly six digits, as the bridge's fixed PIN must be (Link spec 7.13). */
export function pinIsValid(pin: string): boolean {
  return /^[0-9]{6}$/.test(pin);
}

/** Keeps only digits, at most six, so a PIN field cannot hold anything else. */
export function digitsOnly(text: string): string {
  return text.replace(/[^0-9]/g, "").slice(0, PIN_LENGTH);
}
