/*
 * The device page's meter strip: one bar per channel, inputs first, from the
 * combined status frame (REQ_GET_STATUS wValue 9) the hub pushes as POLL
 * frames (Link spec 7.6 and 8.4). A clipped channel says so in words as well
 * as in red (bridge spec 10.4).
 */

import type { StatusFrame } from "@dspi/link";
import { clear, el } from "../dom.js";

/** Peaks are 0..32767, the same scale the firmware uses everywhere. */
const PEAK_FULL_SCALE = 32767;
/** Bottom of the meter scale in dBFS. */
const METER_FLOOR_DB = -60;

/** What createMeters returns. */
export interface Meters {
  root: HTMLElement;
  /** Draws one empty bar per channel. */
  build(inputs: number, outputs: number): void;
  /** Shows one status frame. */
  update(frame: StatusFrame): void;
}

/**
 * Builds the meters panel, with no bars until build() is called.
 *
 * @returns The panel and its controls.
 */
export function createMeters(): Meters {
  const cpuText = el("span", { class: "stat dim", text: "CPU -- / --" });
  const clipText = el("span", { class: "stat dim", text: "" });
  const strip = el("div", { class: "meters" });
  let bars: HTMLElement[] = [];
  let cells: HTMLElement[] = [];
  let labels: string[] = [];
  /** What the clip line says now; it is rewritten only when that changes,
   * since it is read out each time it is. */
  let clipWords = "";

  const root = el(
    "div",
    { class: "panel" },
    el(
      "div",
      { class: "row spread" },
      el("h2", { text: "Meters", style: "margin:0" }),
      // The CPU figures change ten times a second, so they stay out of the
      // live region; only a change in clipping is read out.
      el("span", { class: "row" }, cpuText, el("span", { role: "status" }, clipText)),
    ),
    strip,
  );

  return {
    root,
    build(inputs: number, outputs: number): void {
      clear(strip);
      clipWords = "";
      clipText.textContent = "";
      labels = channelLabels(inputs, outputs);
      bars = [];
      cells = [];
      for (const label of labels) {
        const fill = el("div", { class: "fill" });
        const cell = el(
          "div",
          { class: "meter" },
          el("div", { class: "track" }, fill),
          el("div", { class: "label", text: label }),
          el("div", { class: "clip-mark", text: "Clip" }),
        );
        bars.push(fill);
        cells.push(cell);
        strip.appendChild(cell);
      }
    },
    update(frame: StatusFrame): void {
      const clipped: string[] = [];
      for (let channel = 0; channel < bars.length; channel++) {
        const percent = dbToPercent(peakToDb(frame.peaks[channel] ?? 0));
        bars[channel].style.height = `${percent.toFixed(1)}%`;
        const isClipped = (frame.clipFlags & (1 << channel)) !== 0;
        cells[channel].classList.toggle("clipped", isClipped);
        if (isClipped) clipped.push(labels[channel]);
      }
      cpuText.textContent = `CPU ${frame.cpu0}% / ${frame.cpu1}%`;
      const words = clipped.length ? `Clipping: ${clipped.join(", ")}` : "";
      if (words !== clipWords) {
        clipWords = words;
        clipText.textContent = words;
        clipText.className = clipped.length ? "stat error" : "stat dim";
      }
    },
  };
}

/** Channel names: inputs first, then outputs, matching the unified channel model. */
function channelLabels(inputs: number, outputs: number): string[] {
  const labels: string[] = [];
  for (let input = 1; input <= inputs; input++) labels.push(`In ${input}`);
  for (let output = 1; output <= outputs; output++) labels.push(`Out ${output}`);
  return labels;
}

function dbToPercent(db: number): number {
  if (!Number.isFinite(db)) return 0;
  const clamped = Math.max(METER_FLOOR_DB, Math.min(0, db));
  return ((clamped - METER_FLOOR_DB) / -METER_FLOOR_DB) * 100;
}

function peakToDb(peak: number): number {
  if (peak <= 0) return -Infinity;
  return 20 * Math.log10(peak / PEAK_FULL_SCALE);
}
