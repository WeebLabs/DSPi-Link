/*
 * The status line both pages use: a short title and an optional detail on a
 * tinted background. The words carry the meaning and the colour only helps,
 * as bridge spec 10.4 requires.
 */

import { el } from "./dom.js";

/** How a status line is tinted. */
export type Tone = "good" | "warn" | "bad";

/**
 * Builds a status line.
 *
 * @param tone The tint.
 * @param title What happened, in a few words.
 * @param detail What it means or what to do, if anything.
 * @param announce True to have screen readers read it out at once (role
 *   "alert"); use it only when the line is new, not on every redraw.
 * @returns The element.
 */
export function statusLine(
  tone: Tone,
  title: string,
  detail?: string,
  announce = false,
): HTMLElement {
  return el(
    "div",
    { class: `status ${tone}`, role: announce ? "alert" : null },
    el(
      "div",
      {},
      el("strong", { text: title }),
      detail ? el("span", { class: "detail", text: detail }) : null,
    ),
  );
}

/** What the bridge says about a DSPi it found but cannot serve (bridge spec 7.6). */
export interface UnusableFirmware {
  too_old?: boolean;
  too_new?: boolean;
  /** The DSPi's firmware version, when the bridge could read it. */
  fw?: string;
}

/**
 * The status line for a DSPi whose firmware the bridge cannot serve.
 *
 * @param dspi What the bridge reported.
 * @returns The line, or null when the firmware is fine or nothing answered.
 */
export function unusableFirmwareLine(dspi: UnusableFirmware | undefined): HTMLElement | null {
  if (dspi?.too_old) {
    const version = dspi.fw ? `It runs firmware ${dspi.fw}. ` : "";
    return statusLine(
      "warn",
      "DSPi firmware too old",
      `${version}The bridge needs firmware 1.1.7 or later. ` +
        "Update the DSPi over USB with DSPi Console.",
    );
  }
  if (dspi?.too_new) {
    return statusLine(
      "warn",
      "DSPi firmware too new",
      "The DSPi runs firmware this bridge doesn't understand. Update the bridge.",
    );
  }
  return null;
}
