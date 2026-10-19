/*
 * The settings page's Status section (bridge spec 10.3): whether the DSPi is
 * answering, the link and Wi-Fi health, and the bridge's firmware, from
 * hub.stats (bridge spec 8.7). The page refreshes it every few seconds.
 */

import { el } from "../dom.js";
import { chipLabel } from "../firmware.js";
import { statusLine } from "../status.js";
import { signalWords } from "../wifi.js";
import { count, formatBaud, formatDuration } from "./fields.js";
import { readBridgeStats, type BridgeStats, type SectionContext } from "./state.js";

/**
 * Draws the Status section from the state.
 *
 * @param context The page's context.
 */
export function drawStatus(context: SectionContext): void {
  const { state, options } = context;
  if (options.announce) {
    context.add("status", statusLine(options.announce.tone, options.announce.text));
  }
  if (state.loadError) {
    context.add(
      "status",
      statusLine("bad", "Couldn't read the bridge's settings.", state.loadError),
    );
    return;
  }
  context.add("status", deviceLine(context), notificationsLine(state.stats));
  context.add("status", facts(context));
}

/** Whether the DSPi answers, and which one it is. */
function deviceLine(context: SectionContext): HTMLElement {
  const device = context.client.deviceList[0];
  if (!device || device.state !== "online") {
    return statusLine(
      "warn",
      "DSPi not answering",
      "Check the three wires, and that the UART control interface is on in DSPi Console.",
    );
  }
  const firmware = device.fw
    ? `firmware ${device.fw}${device.fw_beta ? ` beta ${device.fw_beta}` : ""}`
    : null;
  const detail = [`Serial ${device.serial}`, firmware].filter(Boolean).join(", ");
  return statusLine("good", "DSPi connected", detail);
}

/** A warning when the DSPi answers but sends no notifications. */
function notificationsLine(stats: BridgeStats | null): HTMLElement | null {
  const uart = stats?.uart;
  if (!uart || !uart.live || uart.notify_enable) return null;
  return statusLine(
    "warn",
    "DSPi notifications are off",
    "Turn them on in DSPi Console's UART settings so changes show up straight away.",
  );
}

/** The list of facts: link, Wi-Fi, address, firmware and uptime. */
function facts(context: SectionContext): HTMLElement {
  const stats = context.state.stats;
  const list = el("dl", { class: "facts" });
  const fact = (term: string, value: string): void => {
    list.append(el("dt", { text: term }), el("dd", { text: value }));
  };
  if (stats) {
    fact("Link to the DSPi", `${formatBaud(stats.uart.baud)} baud, ${linkHealth(stats.uart)}`);
  }
  if (stats?.wifi.ssid) {
    const signal = `${signalWords(stats.wifi.rssi)} signal (${stats.wifi.rssi} dBm)`;
    fact("Wi-Fi", `${stats.wifi.ssid}, ${signal}`);
    fact("Address", stats.wifi.ip);
  }
  const version = context.client.hub?.hub.version;
  const chip = stats ? chipLabel(stats.chip) : null;
  fact("Bridge firmware", [version, chip].filter(Boolean).join(" on "));
  if (stats) fact("Running for", formatDuration(stats.uptime_s));
  return list;
}

/** The link's errors since the bridge started, in words. */
function linkHealth(uart: BridgeStats["uart"]): string {
  if (!uart.live) return "no answer";
  if (uart.timeouts + uart.crc_errors === 0) return "no errors";
  const timeouts = count(uart.timeouts, "timeout");
  const damaged = count(uart.crc_errors, "damaged reply", "damaged replies");
  return `${timeouts} and ${damaged} since starting`;
}

/**
 * Reads fresh stats and redraws the Status section. A failed refresh keeps
 * the old numbers on screen: the next one, a few seconds later, may work, and
 * a lost connection is reported by the control page.
 *
 * @param context The page's context.
 */
export async function refreshStats(context: SectionContext): Promise<void> {
  const { client, state } = context;
  if (state.leaving || state.disposed || client.state !== "ready") return;
  try {
    const reply = await client.hubStats();
    state.stats = readBridgeStats(reply.bridge);
    if (!state.disposed) context.draw("status");
  } catch (error) {
    console.warn("settings: stats refresh failed:", error);
  }
}
