/*
 * The device page: master volume and a live meter strip for one DSPi.
 *
 * Everything goes through the Link client: the volume is a CMD tunnel (Link
 * spec 8.2, volume.ts), and the meters are a poll subscription on
 * MEAS.LEVELS (0x0910) whose verbatim responses come back as POLL
 * frames (Link spec 7.6, meters.ts). Other clients' changes arrive as NOTIFY
 * frames (Link spec 8.3), and RESYNC (8.6) or BULK_INVALIDATED re-read the
 * state. Nothing here is Link-specific except the transport; the decoding is
 * the same code a USB client would run.
 */

import {
  BULK_HEADER_BYTES,
  BULK_MASTER_VOLUME_OFFSET,
  BULK_PARAMS_BYTES,
  Command,
  LinkClient,
  LinkStatus,
  type DeviceInfo,
  type NotifyFrame,
  type PollFrame,
  type ResyncFrame,
  decodeBulkHeader,
  decodeNotifyPacket,
  decodeStatusFrame,
  isSupportedBulkVersion,
  commandAddress,
  readMasterVolumeDb,
  statusFrameLength,
} from "@dspi/link";
import { el } from "../dom.js";
import { describeProblem } from "../problems.js";
import { createMeters } from "./meters.js";
import { createVolumeControl, readDeviceVolume, readFloat } from "./volume.js";

const METER_SLOT = 0;
const METER_RATE_HZ = 10;
const FLOAT32_BYTES = 4;

/** When neither the hub nor a snapshot gives the channel count: an RP2350's,
 * the larger platform, so no channel goes unshown. */
const FALLBACK_CHANNELS = 17;
/** Inputs to assume when the device's own layout could not be read. */
const FALLBACK_INPUTS = 8;
/** The largest GET a CMD can ask for (wLength is a u16). */
const MAX_GET_BYTES = 0xffff;

/** What createDevicePage returns. */
export interface DevicePageHandles {
  handle: number;
  root: HTMLElement;
  /** Removes the client listeners and ends the meter subscription. */
  dispose(): void;
}

/**
 * A one-line description of a device: serial, firmware and link.
 *
 * @param device The device.
 * @returns "E46058388B1A2E2C  |  fw 1.1.7  |  USB".
 */
export function deviceSummary(device: DeviceInfo): string {
  const parts = [device.serial];
  if (device.fw) parts.push(`fw ${device.fw}${device.fw_beta ? `-beta${device.fw_beta}` : ""}`);
  if (device.link) parts.push(device.link.toUpperCase());
  return parts.join("  |  ");
}

/**
 * Builds the device page and starts reading the device.
 *
 * @param client A ready session.
 * @param device The device to show.
 * @param onBack Called when the user leaves the page.
 * @returns The page's root element and its dispose function.
 */
export function createDevicePage(
  client: LinkClient,
  device: DeviceInfo,
  onBack: () => void,
): DevicePageHandles {
  const handle = device.handle;
  let inputs = device.inputs ?? 0;
  let outputs = device.outputs ?? 0;
  // A hub may leave `inputs` out (Link spec 7.4), and the status frame starts
  // with the inputs, so both counts are needed before the meters mean anything.
  let layoutKnown = device.inputs !== undefined && device.outputs !== undefined;
  let disposed = false;
  // False once a snapshot shows a wire version the bulk offsets were not checked against.
  let bulkOffsetsValid = true;
  // The poll `len` the hub was last asked for, or null before the first subscription.
  let subscribedLength: number | null = null;

  const status = el("p", { class: "dim", role: "status", text: "Reading the DSPi…" });
  const setStatus = (text: string, isProblem = false): void => {
    status.textContent = text;
    status.className = isProblem ? "error" : "dim";
  };
  const volume = createVolumeControl(client, handle, (text) => setStatus(text, true));
  const meters = createMeters();

  /**
   * Reads the channel layout and the volume: from the hub's cache if it has
   * one, else from the device's own bulk blob when the layout is not known,
   * and the volume with a GET when neither gave it.
   */
  async function loadState(): Promise<void> {
    let volumeShown = false;
    if (client.hasCap("snapshot")) {
      const snapshot = await client.snapshot(handle);
      if (snapshot.bulk) volumeShown = applyBulk(snapshot.bulk);
    }
    if (!layoutKnown) {
      const blob = await readBulk();
      if (blob) volumeShown = applyBulk(blob);
    }
    if (!volumeShown) {
      const db = await readDeviceVolume(client, handle);
      if (db !== null) volume.showDeviceVolume(db);
    }
    if (!layoutKnown) {
      inputs = FALLBACK_INPUTS;
      outputs = outputs || FALLBACK_CHANNELS - FALLBACK_INPUTS;
    }
  }

  /**
   * Takes the layout from a bulk blob's header, and the volume when the blob's
   * wire version is one the offsets were checked against.
   *
   * @returns True when the volume was shown.
   */
  function applyBulk(blob: Uint8Array): boolean {
    if (blob.length <= BULK_HEADER_BYTES) return false;
    const header = decodeBulkHeader(blob);
    inputs = header.numInputChannels;
    outputs = header.numOutputChannels;
    layoutKnown = true;
    bulkOffsetsValid = isSupportedBulkVersion(header);
    if (!bulkOffsetsValid) return false;
    volume.showDeviceVolume(readMasterVolumeDb(blob));
    return true;
  }

  /** The device's bulk blob (STORE.ALL_PARAMS), whose header gives the channel
   * layout, or null if it cannot be had. */
  async function readBulk(): Promise<Uint8Array | null> {
    const length = Math.min(client.limits?.max_payload ?? BULK_PARAMS_BYTES, MAX_GET_BYTES);
    try {
      const response = await client.get(handle, Command.STORE_ALL_PARAMS, length);
      return response.status === LinkStatus.OK ? response.payload : null;
    } catch (error) {
      console.warn("device: could not read the bulk blob:", error);
      return null;
    }
  }

  /** Asks for the meter feed, again whenever the channel count changes its length. */
  async function subscribeMeters(): Promise<void> {
    const length = statusFrameLength(inputs + outputs);
    if (length === subscribedLength) return;
    subscribedLength = length;
    const { bRequest, wIndex } = commandAddress(Command.MEAS_LEVELS);
    await client.subscribePolls(handle, [
      { slot: METER_SLOT, req: bRequest, val: 0, idx: wIndex, len: length, hz: METER_RATE_HZ },
    ]);
  }

  // One refresh at a time: a RESYNC and a BULK_INVALIDATED often come
  // together, and a hub answers a second snapshot request `busy` while the
  // first waits (bridge spec 8.10). One that arrives meanwhile runs after.
  let refreshing = false;
  let refreshAgain = false;

  function refresh(): void {
    if (refreshing) {
      refreshAgain = true;
      return;
    }
    refreshing = true;
    void (async () => {
      do {
        refreshAgain = false;
        await refreshOnce();
      } while (refreshAgain && !disposed);
      refreshing = false;
    })();
  }

  /** Reads the state, redraws the meters, and keeps the subscription in step. */
  async function refreshOnce(): Promise<void> {
    try {
      await loadState();
      if (disposed) return;
      meters.build(inputs, outputs);
      setStatus(`${inputs} inputs, ${outputs} outputs`);
    } catch (error) {
      setStatus(describeProblem(error, "DSPi"), true);
      return;
    }
    if (disposed) return;
    try {
      await subscribeMeters();
    } catch (error) {
      subscribedLength = null;
      setStatus(`The meters are unavailable. ${describeProblem(error, "hub")}`, true);
    }
  }

  function onPoll(frame: PollFrame): void {
    if (frame.handle !== handle || frame.slot !== METER_SLOT) return;
    try {
      meters.update(decodeStatusFrame(frame.payload, inputs + outputs));
    } catch {
      // A frame sized for an old channel count, still in flight after a
      // resubscribe; the next one fits.
    }
  }

  function onNotify(frame: NotifyFrame): void {
    if (frame.handle !== handle) return;
    // Our own writes come back too; drop them by origin, never by source, since
    // over Link HOST_SET only means "some client" (Link spec 8.3).
    if (client.session !== null && frame.origin === client.session) return;
    const packet = decodeNotifyPacket(frame.packet);
    if (packet?.kind === "bulk_invalidated") {
      refresh();
      return;
    }
    if (packet?.kind !== "param_changed" || !bulkOffsetsValid) return;
    if (packet.wireOffset === BULK_MASTER_VOLUME_OFFSET && packet.value.length >= FLOAT32_BYTES) {
      volume.showDeviceVolume(readFloat(packet.value));
    }
  }

  function onResync(frame: ResyncFrame): void {
    if (frame.handle === handle) refresh();
  }

  const stopListening = [
    client.on("poll", onPoll),
    client.on("notify", onNotify),
    client.on("resync", onResync),
  ];

  const root = el(
    "div",
    {},
    el(
      "div",
      { class: "row spread", style: "margin-bottom:16px" },
      el("button", { type: "button", text: "Back", onClick: onBack }),
      el("span", { class: "badge", text: client.role ? `Role: ${client.role}` : "" }),
    ),
    el(
      "div",
      { class: "panel" },
      el("h1", { text: device.name }),
      el("p", { class: "dim mono", text: deviceSummary(device) }),
      status,
    ),
    // A legacy device takes no paged command, so it gets no controls.
    device.protocol ? volume.root : null,
    device.protocol ? meters.root : null,
  );

  meters.build(inputs, outputs);
  if (device.protocol) {
    refresh();
  } else {
    // Firmware before 1.1.7 numbers its commands differently, and the hub
    // forwards nothing to it (Link spec 7.4).
    setStatus(
      "This DSPi's firmware is too old for DSPi Link. Update it over USB with DSPi Console.",
      true,
    );
  }

  return {
    handle,
    root,
    dispose(): void {
      disposed = true;
      for (const stop of stopListening) stop();
      volume.dispose();
      // Hubs must stop polling when nobody listens, so say so on the way out.
      client.unsubscribePolls(handle, [METER_SLOT]).catch((error: unknown) => {
        console.warn("device: unsubscribe failed:", error);
      });
    },
  };
}
