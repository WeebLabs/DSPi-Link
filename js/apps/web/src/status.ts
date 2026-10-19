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
