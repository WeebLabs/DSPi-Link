/*
 * The device page's master volume: a slider and a mute button.
 *
 * Reads and writes are REQ_GET/SET_MASTER_VOLUME (0xD3/0xD2) tunnelled as CMD
 * frames (Link spec 8.2), exactly as over USB. Writes while dragging are
 * throttled, and the last value of a burst always reaches the device.
 */

import { LinkClient, LinkStatus, MASTER_VOLUME_MUTE_DB } from "@dspi/link";
import { el } from "../dom.js";
import { describeProblem } from "../problems.js";

/** REQ_SET_MASTER_VOLUME. */
export const REQ_SET_MASTER_VOLUME = 0xd2;
/** REQ_GET_MASTER_VOLUME. */
export const REQ_GET_MASTER_VOLUME = 0xd3;
/** wIndex for application commands (Link spec 8.2). */
export const APP_WINDEX = 2;

/** The slider's range; below it is the firmware's mute sentinel. */
const VOLUME_MIN_DB = -127;
const VOLUME_MAX_DB = 0;
const VOLUME_STEP_DB = 0.5;
/** Shown until the device reports its level. */
const VOLUME_INITIAL_DB = -20;
/** About 20 writes a second while the slider is dragged; the spec sets no rate. */
const VOLUME_WRITE_INTERVAL_MS = 50;
const FLOAT32_BYTES = 4;

/** What createVolumeControl returns. */
export interface VolumeControl {
  root: HTMLElement;
  /** Shows a level the device reported, without echoing it back. */
  showDeviceVolume(db: number): void;
  /** Stops a pending write. */
  dispose(): void;
}

/**
 * Builds the volume panel.
 *
 * @param client The session.
 * @param handle The device.
 * @param onProblem Called with a sentence when a write fails.
 * @returns The panel and its controls.
 */
export function createVolumeControl(
  client: LinkClient,
  handle: number,
  onProblem: (text: string) => void,
): VolumeControl {
  // The level to return to when unmuting, since the device reports mute as a
  // sentinel and forgets the level behind it.
  let levelDb = VOLUME_INITIAL_DB;
  let muted = false;
  let dragging = false;
  let disposed = false;

  const value = el("div", { class: "volume-value", id: "volume-value", text: "-- dB" });
  const slider = el("input", {
    type: "range",
    min: String(VOLUME_MIN_DB),
    max: String(VOLUME_MAX_DB),
    step: String(VOLUME_STEP_DB),
    value: String(VOLUME_INITIAL_DB),
    "aria-labelledby": "volume-title",
  });
  const muteButton = el("button", { type: "button", text: "Mute", "aria-pressed": "false" });

  const render = (): void => {
    value.textContent = muted ? "Muted" : `${levelDb.toFixed(1)} dB`;
    muteButton.textContent = muted ? "Unmute" : "Mute";
    muteButton.setAttribute("aria-pressed", String(muted));
    muteButton.classList.toggle("primary", muted);
    slider.setAttribute("aria-valuetext", muted ? "muted" : `${levelDb.toFixed(1)} dB`);
    if (!dragging) slider.value = String(levelDb);
  };

  const writer = createThrottledWriter((db) => {
    if (disposed) return;
    client
      .set(handle, REQ_SET_MASTER_VOLUME, floatBytes(db), 0, APP_WINDEX)
      .then((response) => {
        if (response.status !== LinkStatus.OK) onProblem(refusal(response.status));
      })
      .catch((error: unknown) => onProblem(describeProblem(error)));
  });

  slider.addEventListener("pointerdown", () => (dragging = true));
  slider.addEventListener("pointerup", () => (dragging = false));
  slider.addEventListener("input", () => {
    dragging = true;
    levelDb = Number(slider.value);
    muted = false;
    render();
    writer.queue(levelDb);
  });
  slider.addEventListener("change", () => {
    dragging = false;
    // Where the user let go, not levelDb: another client's change can land
    // during the drag, and must not be what this one sends last.
    levelDb = Number(slider.value);
    muted = false;
    render();
    writer.queue(levelDb);
  });
  muteButton.addEventListener("click", () => {
    muted = !muted;
    render();
    writer.queue(muted ? MASTER_VOLUME_MUTE_DB : levelDb);
  });

  const root = el(
    "div",
    { class: "panel" },
    el("h2", { id: "volume-title", text: "Master volume" }),
    el("div", { class: "row spread" }, value, muteButton),
    slider,
  );

  return {
    root,
    showDeviceVolume(db: number): void {
      if (db <= MASTER_VOLUME_MUTE_DB) {
        muted = true;
      } else {
        muted = false;
        levelDb = Math.max(VOLUME_MIN_DB, Math.min(VOLUME_MAX_DB, db));
      }
      render();
    },
    dispose(): void {
      disposed = true;
      writer.cancel();
    },
  };
}

/**
 * Reads the master volume straight from the device.
 *
 * @param client The session.
 * @param handle The device.
 * @returns The level in dB, or null when the device did not answer OK.
 */
export async function readDeviceVolume(client: LinkClient, handle: number): Promise<number | null> {
  const response = await client.get(handle, REQ_GET_MASTER_VOLUME, FLOAT32_BYTES, 0, APP_WINDEX);
  if (response.status !== LinkStatus.OK || response.payload.length < FLOAT32_BYTES) return null;
  return readFloat(response.payload);
}

/**
 * @param bytes At least four bytes.
 * @returns The float32 LE at their start.
 */
export function readFloat(bytes: Uint8Array): number {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getFloat32(0, true);
}

function floatBytes(value: number): Uint8Array {
  const bytes = new Uint8Array(FLOAT32_BYTES);
  new DataView(bytes.buffer).setFloat32(0, value, true);
  return bytes;
}

/** Why the device refused a volume write, in words. */
function refusal(status: number): string {
  switch (status) {
    case LinkStatus.LOCKED:
      return "Another app is applying changes to this DSPi. Try again in a moment.";
    case LinkStatus.DENIED:
      return "This browser can watch this DSPi but not change its volume.";
    case LinkStatus.BUSY:
    case LinkStatus.TIMEOUT:
      return "The DSPi didn't answer in time. Try again.";
    default:
      return "The DSPi didn't accept the volume change.";
  }
}

/** A trailing throttle: at most one write per interval, and the last value always goes. */
function createThrottledWriter(write: (db: number) => void): {
  queue(db: number): void;
  cancel(): void;
} {
  let pending: number | null = null;
  let timer: ReturnType<typeof setTimeout> | null = null;
  let lastWriteAt = 0;

  const flush = (): void => {
    timer = null;
    if (pending === null) return;
    const db = pending;
    pending = null;
    lastWriteAt = Date.now();
    write(db);
  };

  return {
    queue(db: number): void {
      pending = db;
      if (timer !== null) return;
      const sinceMs = Date.now() - lastWriteAt;
      if (sinceMs >= VOLUME_WRITE_INTERVAL_MS) flush();
      else timer = setTimeout(flush, VOLUME_WRITE_INTERVAL_MS - sinceMs);
    },
    cancel(): void {
      if (timer !== null) clearTimeout(timer);
      timer = null;
      pending = null;
    },
  };
}
