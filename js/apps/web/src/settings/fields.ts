/*
 * The form controls and wording helpers every settings section shares
 * (bridge spec 10.3), so each field has a real label (bridge spec 10.4) and
 * each error reads as a sentence the user can act on.
 */

import { LinkProtocolError } from "@dspi/link";
import { el } from "../dom.js";
import { PIN_LENGTH, digitsOnly } from "../pin.js";
import { describeProblem } from "../problems.js";

/** One option of a select field. */
export interface Choice {
  value: string | number;
  label: string;
}

/** Names for the keys a rejected hub.config.set can name (Link spec 7.10). */
const FIELD_LABELS: Record<string, string> = {
  name: "name",
  hostname: "hostname",
  "uart.tx_pin": "TX pin",
  "uart.rx_pin": "RX pin",
  "uart.baud": "baud rate",
  "uart.detect_baud": "automatic speed setting",
  "led.type": "LED type",
  "led.pin": "LED pin",
};

const SECONDS_PER_MINUTE = 60;
const MINUTES_PER_HOUR = 60;
const HOURS_PER_DAY = 24;

/**
 * GPIO numbers as select choices.
 *
 * @param pins The pins the bridge allows.
 * @returns One choice per pin, labelled "GPIO n".
 */
export function pinChoices(pins: number[]): Choice[] {
  return pins.map((pin) => ({ value: pin, label: `GPIO ${pin}` }));
}

/**
 * A labelled select.
 *
 * @param id The select's id, which its label points at.
 * @param label The label.
 * @param choices The options.
 * @param selected The value to show selected.
 * @param onChange Called with the new value.
 * @param help A line of explanation under the field.
 * @returns The field's element.
 */
export function selectField(
  id: string,
  label: string,
  choices: Choice[],
  selected: string | number,
  onChange: (value: string) => void,
  help?: string,
): HTMLElement {
  const select = el("select", {
    id,
    onChange: (event: Event) => onChange((event.target as HTMLSelectElement).value),
  });
  for (const choice of choices) {
    select.appendChild(
      el("option", {
        value: String(choice.value),
        text: choice.label,
        selected: choice.value === selected,
      }),
    );
  }
  return el(
    "div",
    { class: "field" },
    el("label", { for: id, text: label }),
    select,
    help ? el("p", { class: "dim small", text: help }) : null,
  );
}

/**
 * A labelled one-line text field.
 *
 * @param id The input's id, which its label points at.
 * @param label The label.
 * @param value The text to start with.
 * @param maxLength The most characters it takes.
 * @param onInput Called with the text after every change.
 * @param help A line of explanation under the field.
 * @returns The field's element.
 */
export function textField(
  id: string,
  label: string,
  value: string,
  maxLength: number,
  onInput: (value: string) => void,
  help?: string,
): HTMLElement {
  return el(
    "div",
    { class: "field" },
    el("label", { for: id, text: label }),
    el("input", {
      type: "text",
      id,
      value,
      maxLength,
      autocapitalize: "off",
      spellcheck: false,
      onInput: (event: Event) => onInput((event.target as HTMLInputElement).value),
    }),
    help ? el("p", { class: "dim small", text: help }) : null,
  );
}

/**
 * A checkbox with its label beside it.
 *
 * @param id The checkbox's id.
 * @param label The label.
 * @param checked Whether it starts ticked.
 * @param onChange Called with the new state.
 * @returns The field's element.
 */
export function checkboxField(
  id: string,
  label: string,
  checked: boolean,
  onChange: (checked: boolean) => void,
): HTMLElement {
  return el(
    "label",
    { class: "checkbox", for: id },
    el("input", {
      type: "checkbox",
      id,
      checked,
      onChange: (event: Event) => onChange((event.target as HTMLInputElement).checked),
    }),
    el("span", { text: label }),
  );
}

/**
 * A 6-digit PIN field that holds only digits.
 *
 * @param id The input's id.
 * @param label The label.
 * @param value The digits to start with.
 * @param onInput Called with the digits after every change.
 * @returns The field's element.
 */
export function pinInput(
  id: string,
  label: string,
  value: string,
  onInput: (value: string) => void,
): HTMLElement {
  return el(
    "div",
    { class: "field" },
    el("label", { for: id, text: label }),
    el("input", {
      type: "password",
      id,
      class: "pin",
      value,
      inputMode: "numeric",
      pattern: "[0-9]*",
      autocomplete: "off",
      maxLength: PIN_LENGTH,
      onInput: (event: Event) => {
        const field = event.target as HTMLInputElement;
        field.value = digitsOnly(field.value);
        onInput(field.value);
      },
    }),
  );
}

/**
 * One row of the Maintenance section: what it does, and its button.
 *
 * @param title The action's name.
 * @param explanation What it does and what it keeps.
 * @param action The button, or whatever stands in for it.
 * @returns The row's element.
 */
export function maintenanceRow(
  title: string,
  explanation: string,
  action: HTMLElement,
): HTMLElement {
  return el(
    "div",
    { class: "maintenance-row" },
    el(
      "div",
      { class: "grow" },
      el("strong", { text: title }),
      el("p", { class: "dim small", text: explanation }),
    ),
    action,
  );
}

/**
 * A sentence the user can act on, from whatever a request threw.
 *
 * @param error What the request rejected with.
 * @returns The sentence.
 */
export function explain(error: unknown): string {
  if (error instanceof LinkProtocolError) {
    if (error.field) {
      return `The bridge didn't accept the ${FIELD_LABELS[error.field] ?? error.field}.`;
    }
    if (error.code === "denied") return "Only an admin can change the bridge's settings.";
    if (error.code === "busy") {
      return "The bridge is busy with another change. Try again in a moment.";
    }
  }
  return describeProblem(error, "bridge");
}

/**
 * Whether two plain values hold the same data.
 *
 * @param first One value.
 * @param second The other.
 * @returns True when their JSON is the same.
 */
export function sameJson(first: unknown, second: unknown): boolean {
  return JSON.stringify(first) === JSON.stringify(second);
}

/**
 * A baud rate with thousands separators, "1,000,000".
 *
 * @param baud The rate.
 * @returns The rate as text.
 */
export function formatBaud(baud: number): string {
  return baud.toLocaleString("en-US");
}

/**
 * "1 timeout", "3 timeouts".
 *
 * @param amount How many.
 * @param one The word for one.
 * @param many The word for several.
 * @returns The phrase.
 */
export function count(amount: number, one: string, many = `${one}s`): string {
  return `${amount} ${amount === 1 ? one : many}`;
}

/**
 * How long the bridge has been running, in words.
 *
 * @param seconds The uptime.
 * @returns "2 days, 3 hours", "4 hours, 10 minutes" or "5 minutes".
 */
export function formatDuration(seconds: number): string {
  const minutes = Math.floor(seconds / SECONDS_PER_MINUTE);
  const hours = Math.floor(minutes / MINUTES_PER_HOUR);
  const days = Math.floor(hours / HOURS_PER_DAY);
  if (days > 0) return `${count(days, "day")}, ${count(hours % HOURS_PER_DAY, "hour")}`;
  if (hours > 0) return `${count(hours, "hour")}, ${count(minutes % MINUTES_PER_HOUR, "minute")}`;
  return count(minutes, "minute");
}

/**
 * "5 minutes ago", from an ISO 8601 time.
 *
 * @param iso The time.
 * @returns The phrase, or "at an unknown time" if it cannot be read.
 */
export function timeAgo(iso: string): string {
  const seconds = (Date.parse(iso) - Date.now()) / 1000;
  if (!Number.isFinite(seconds)) return "at an unknown time";
  const format = new Intl.RelativeTimeFormat("en", { numeric: "auto" });
  const units: Array<[Intl.RelativeTimeFormatUnit, number]> = [
    ["day", SECONDS_PER_MINUTE * MINUTES_PER_HOUR * HOURS_PER_DAY],
    ["hour", SECONDS_PER_MINUTE * MINUTES_PER_HOUR],
    ["minute", SECONDS_PER_MINUTE],
  ];
  for (const [unit, size] of units) {
    if (Math.abs(seconds) >= size) return format.format(Math.round(seconds / size), unit);
  }
  return "just now";
}
