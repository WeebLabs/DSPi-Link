/*
 * The device page: master volume and a live meter strip.
 *
 * Everything here goes through the Link client: the volume is REQ_SET/GET_
 * MASTER_VOLUME (0xD2/0xD3) tunnelled as CMD frames, and the meters are a poll
 * subscription on REQ_GET_STATUS (0x50, wValue 9) whose verbatim responses come
 * back as POLL frames. Nothing about this page is Link-specific except the
 * transport; the decoding is the same code a USB client would run.
 */

import {
  BULK_MASTER_VOLUME_OFFSET,
  LinkClient,
  LinkStatus,
  MASTER_VOLUME_MUTE_DB,
  type DeviceInfo,
  decodeBulkHeader,
  decodeNotifyPacket,
  decodeStatusFrame,
  isSupportedBulkVersion,
  readMasterVolumeDb,
  statusFrameLength,
} from "@dspi/link";
import { clear, el } from "./dom.js";

const REQ_GET_STATUS = 0x50;
const REQ_SET_MASTER_VOLUME = 0xd2;
const REQ_GET_MASTER_VOLUME = 0xd3;
const STATUS_WVALUE_COMBINED = 9;
const APP_WINDEX = 2;

const METER_SLOT = 0;
const METER_HZ = 10;
/** Peaks are 0..32767, the same scale the firmware uses everywhere. */
const PEAK_FULL_SCALE = 32767;
/** Bottom of the meter scale in dBFS. */
const METER_FLOOR_DB = -60;
/** ~20 writes a second while a slider is being dragged (spec has no rate rule). */
const VOLUME_WRITE_INTERVAL_MS = 50;

export interface DevicePageHandles {
  handle: number;
  root: HTMLElement;
  dispose(): void;
}

export function deviceHandleLabel(device: DeviceInfo): string {
  const bits = [device.serial];
  if (device.fw) bits.push(`fw ${device.fw}${device.fw_beta ? `-beta${device.fw_beta}` : ""}`);
  if (device.link) bits.push(device.link.toUpperCase());
  return bits.join("  |  ");
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

function floatBytes(value: number): Uint8Array {
  const bytes = new Uint8Array(4);
  new DataView(bytes.buffer).setFloat32(0, value, true);
  return bytes;
}

/** Channel names: inputs first, then outputs, matching the unified channel model. */
function channelLabels(inputs: number, outputs: number): string[] {
  const labels: string[] = [];
  for (let i = 0; i < inputs; i++) labels.push(`In ${i + 1}`);
  for (let i = 0; i < outputs; i++) labels.push(`Out ${i + 1}`);
  return labels;
}

export function createDevicePage(
  client: LinkClient,
  device: DeviceInfo,
  onBack: () => void,
): DevicePageHandles {
  const handle = device.handle;
  let numChannels = (device.inputs ?? 0) + (device.outputs ?? 0);
  let inputs = device.inputs ?? 0;
  let outputs = device.outputs ?? 0;
  let disposed = false;
  // False once a snapshot shows a wire version the bulk offsets were not checked against.
  let bulkOffsetsValid = true;

  // -- volume state --------------------------------------------------------
  // lastLevelDb is the level to return to when unmuting, since the device
  // reports mute as a sentinel and forgets the level behind it.
  let lastLevelDb = -20;
  let muted = false;
  let dragging = false;
  let pendingWrite: number | null = null;
  let writeTimer: number | null = null;
  let lastWriteAt = 0;

  const status = el("p", { class: "dim" }, "Reading device state...");
  const volumeValue = el("div", { class: "volume-value", text: "-- dB" });
  const slider = el("input", {
    type: "range",
    min: "-127",
    max: "0",
    step: "0.5",
    value: "-20",
  }) as HTMLInputElement;
  const muteButton = el("button", { type: "button", text: "Mute" });
  const cpuText = el("span", { class: "stat dim", text: "CPU -- / --" });
  const clipText = el("span", { class: "stat dim", text: "" });
  const meters = el("div", { class: "meters" });

  let meterBars: HTMLElement[] = [];
  let meterCells: HTMLElement[] = [];

  function buildMeters(): void {
    clear(meters);
    meterBars = [];
    meterCells = [];
    const labels = channelLabels(inputs, outputs);
    for (let i = 0; i < numChannels; i++) {
      const fill = el("div", { class: "fill" });
      const cell = el(
        "div",
        { class: "meter" },
        el("div", { class: "track" }, fill),
        el("div", { class: "label", text: labels[i] ?? `Ch ${i + 1}` }),
      );
      meterBars.push(fill);
      meterCells.push(cell);
      meters.appendChild(cell);
    }
  }

  function renderVolume(): void {
    if (muted) {
      volumeValue.textContent = "MUTE";
      muteButton.textContent = "Unmute";
      muteButton.classList.add("primary");
    } else {
      volumeValue.textContent = `${lastLevelDb.toFixed(1)} dB`;
      muteButton.textContent = "Mute";
      muteButton.classList.remove("primary");
    }
    if (!dragging) slider.value = String(lastLevelDb);
  }

  /** Apply a dB value the device reported, without echoing it back. */
  function applyDeviceVolume(db: number): void {
    if (db <= MASTER_VOLUME_MUTE_DB) {
      muted = true;
    } else {
      muted = false;
      lastLevelDb = Math.max(-127, Math.min(0, db));
    }
    renderVolume();
  }

  // Trailing throttle: the last value in a burst always reaches the device.
  function queueVolumeWrite(db: number): void {
    pendingWrite = db;
    const since = Date.now() - lastWriteAt;
    if (writeTimer !== null) return;
    if (since >= VOLUME_WRITE_INTERVAL_MS) {
      flushVolumeWrite();
      return;
    }
    writeTimer = window.setTimeout(flushVolumeWrite, VOLUME_WRITE_INTERVAL_MS - since);
  }

  function flushVolumeWrite(): void {
    if (writeTimer !== null) {
      clearTimeout(writeTimer);
      writeTimer = null;
    }
    if (pendingWrite === null || disposed) return;
    const db = pendingWrite;
    pendingWrite = null;
    lastWriteAt = Date.now();
    client
      .set(handle, REQ_SET_MASTER_VOLUME, floatBytes(db), 0, APP_WINDEX)
      .then((res) => {
        if (res.status !== LinkStatus.OK) setStatus(`Volume write refused (${res.status})`, true);
      })
      .catch((e: Error) => setStatus(e.message, true));
  }

  function setStatus(text: string, isError = false): void {
    status.textContent = text;
    status.className = isError ? "error" : "dim";
  }

  slider.addEventListener("pointerdown", () => (dragging = true));
  slider.addEventListener("pointerup", () => (dragging = false));
  slider.addEventListener("input", () => {
    dragging = true;
    lastLevelDb = Number(slider.value);
    muted = false;
    renderVolume();
    queueVolumeWrite(lastLevelDb);
  });
  slider.addEventListener("change", () => {
    dragging = false;
    queueVolumeWrite(muted ? MASTER_VOLUME_MUTE_DB : lastLevelDb);
  });
  muteButton.addEventListener("click", () => {
    muted = !muted;
    renderVolume();
    queueVolumeWrite(muted ? MASTER_VOLUME_MUTE_DB : lastLevelDb);
  });

  // -- reading device state -------------------------------------------------

  async function loadState(): Promise<void> {
    try {
      if (client.hasCap("snapshot")) {
        const snap = await client.snapshot(handle);
        if (snap.bulk && snap.bulk.length > 16) {
          const header = decodeBulkHeader(snap.bulk);
          numChannels = header.numChannels;
          inputs = header.numInputChannels;
          outputs = header.numOutputChannels;
          bulkOffsetsValid = isSupportedBulkVersion(header);
          if (bulkOffsetsValid) {
            applyDeviceVolume(readMasterVolumeDb(snap.bulk));
            buildMeters();
            setStatus(
              `Wire version ${header.wireVersion}, ${numChannels} channels, snapshot ${snap.age_ms ?? 0} ms old`,
            );
            return;
          }
        }
      }
      // No cache on this hub, or an empty one: ask the device itself.
      const res = await client.get(handle, REQ_GET_MASTER_VOLUME, 4, 0, APP_WINDEX);
      if (res.status === LinkStatus.OK && res.payload.length >= 4) {
        const dv = new DataView(res.payload.buffer, res.payload.byteOffset, res.payload.byteLength);
        applyDeviceVolume(dv.getFloat32(0, true));
      }
      if (numChannels === 0) numChannels = 17;
      if (inputs + outputs !== numChannels) {
        inputs = Math.min(8, numChannels);
        outputs = numChannels - inputs;
      }
      buildMeters();
      setStatus(`${numChannels} channels`);
    } catch (e) {
      setStatus((e as Error).message, true);
    }
  }

  async function subscribeMeters(): Promise<void> {
    try {
      await client.subscribePolls(handle, [
        {
          slot: METER_SLOT,
          req: REQ_GET_STATUS,
          val: STATUS_WVALUE_COMBINED,
          idx: APP_WINDEX,
          len: statusFrameLength(numChannels),
          hz: METER_HZ,
        },
      ]);
    } catch (e) {
      setStatus(`Meters unavailable: ${(e as Error).message}`, true);
    }
  }

  function onPoll(frame: { handle: number; slot: number; payload: Uint8Array }): void {
    if (frame.handle !== handle || frame.slot !== METER_SLOT) return;
    let decoded;
    try {
      decoded = decodeStatusFrame(frame.payload, numChannels);
    } catch {
      return; // A short frame from a device we sized wrongly; wait for the next.
    }
    for (let i = 0; i < meterBars.length; i++) {
      const percent = dbToPercent(peakToDb(decoded.peaks[i] ?? 0));
      meterBars[i].style.height = `${percent.toFixed(1)}%`;
      meterCells[i].classList.toggle("clipped", (decoded.clipFlags & (1 << i)) !== 0);
    }
    cpuText.textContent = `CPU ${decoded.cpu0}% / ${decoded.cpu1}%`;
    clipText.textContent = decoded.clipFlags ? "CLIP" : "";
    clipText.className = decoded.clipFlags ? "stat error" : "stat dim";
  }

  function onNotify(frame: { handle: number; origin: number; packet: Uint8Array }): void {
    if (frame.handle !== handle) return;
    // Our own writes come back too; drop them by origin, never by source, since
    // over Link HOST_SET only means "some client" (spec 8.3).
    if (client.session !== null && frame.origin === client.session) return;
    const packet = decodeNotifyPacket(frame.packet);
    if (!packet) return;
    if (packet.kind === "param_changed") {
      if (bulkOffsetsValid && packet.wireOffset === BULK_MASTER_VOLUME_OFFSET && packet.value.length >= 4) {
        const dv = new DataView(packet.value.buffer, packet.value.byteOffset, packet.value.byteLength);
        applyDeviceVolume(dv.getFloat32(0, true));
      }
      return;
    }
    if (packet.kind === "bulk_invalidated") void loadState();
  }

  function onResync(frame: { handle: number }): void {
    if (frame.handle === handle) void loadState();
  }

  const offPoll = client.on("poll", onPoll);
  const offNotify = client.on("notify", onNotify);
  const offResync = client.on("resync", onResync);

  const root = el(
    "div",
    {},
    el(
      "div",
      { class: "row spread", style: "margin-bottom:16px" },
      el("button", { type: "button", text: "Back", onClick: onBack }),
      el("span", { class: "badge", text: client.role ?? "" }),
    ),
    el(
      "div",
      { class: "panel" },
      el("h1", { text: device.name }),
      el("p", { class: "dim mono", text: deviceHandleLabel(device) }),
      status,
    ),
    el(
      "div",
      { class: "panel" },
      el("h2", { text: "Master volume" }),
      el("div", { class: "row spread" }, volumeValue, muteButton),
      slider,
    ),
    el(
      "div",
      { class: "panel" },
      el(
        "div",
        { class: "row spread" },
        el("h2", { text: "Meters", style: "margin:0" }),
        el("span", { class: "row" }, cpuText, clipText),
      ),
      meters,
    ),
  );

  buildMeters();
  void loadState().then(() => {
    if (!disposed) void subscribeMeters();
  });

  return {
    handle,
    root,
    dispose(): void {
      disposed = true;
      offPoll();
      offNotify();
      offResync();
      if (writeTimer !== null) clearTimeout(writeTimer);
      // Hubs must stop polling when nobody listens, so say so on the way out.
      client.unsubscribePolls(handle, [METER_SLOT]).catch(() => undefined);
    },
  };
}
