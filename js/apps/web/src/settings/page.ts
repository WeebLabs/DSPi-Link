/*
 * The bridge's settings page (bridge spec 10.3), shown inside the control
 * page when the hub has the `hub_config` capability and this session is
 * `admin`. Everything goes through the Link admin messages (Link spec 7.9 to
 * 7.14); the page has no API of its own.
 *
 *   Status           what the bridge sees now, refreshed every few seconds
 *   DSPi connection  UART pins and speed, with a wiring diagram
 *   Wi-Fi            the current network, and moving to another one
 *   Security         the PIN, and the browsers and apps paired with it
 *   Bridge           name, hostname and status LED
 *   Maintenance      update firmware, restart, forget Wi-Fi, factory reset
 *
 * This file lays the sections out and owns their state; each section draws
 * itself (one file each) into its own element, so redrawing one (new stats, a
 * saved change) never disturbs a field being typed in another.
 */

import type { LinkClient } from "@dspi/link";
import { confirmAction } from "../dialog.js";
import { clear, el } from "../dom.js";
import { statusLine } from "../status.js";
import { drawBridge } from "./bridge.js";
import { drawDspi } from "./dspi.js";
import { explain } from "./fields.js";
import { drawMaintenance } from "./maintenance.js";
import { drawSecurity } from "./security.js";
import {
  initialState,
  readBridgeConfig,
  readBridgeOptions,
  readBridgeStats,
  type SectionContext,
  type SectionName,
  type SettingsPageOptions,
} from "./state.js";
import { drawStatus, refreshStats } from "./status.js";
import { drawWifi } from "./wifi.js";

export type { SettingsPageOptions } from "./state.js";

/** What createSettingsPage returns. */
export interface SettingsPageHandles {
  root: HTMLElement;
  /** Stops the refresh timer and the client listeners, and closes any open dialog. */
  dispose(): void;
}

/** How often the Status section asks for fresh numbers. */
const STATS_INTERVAL_MS = 5000;

const SECTIONS: Array<{
  name: SectionName;
  title: string;
  draw: (context: SectionContext) => void;
}> = [
  { name: "status", title: "Status", draw: drawStatus },
  { name: "dspi", title: "DSPi connection", draw: drawDspi },
  { name: "wifi", title: "Wi-Fi", draw: drawWifi },
  { name: "security", title: "Security", draw: drawSecurity },
  { name: "bridge", title: "Bridge", draw: drawBridge },
  { name: "maintenance", title: "Maintenance", draw: drawMaintenance },
];

const UNREADABLE_SETTINGS = "The bridge sent settings this page doesn't understand.";

/**
 * Builds the settings page and starts loading it.
 *
 * @param client An admin session to a hub with `hub_config`.
 * @param options Where the page reports back to the control page.
 * @returns The page's root element and its dispose function.
 */
export function createSettingsPage(
  client: LinkClient,
  options: SettingsPageOptions,
): SettingsPageHandles {
  const state = initialState(client);
  const dialogs = new AbortController();
  const bodies = new Map<SectionName, HTMLElement>();

  const context: SectionContext = {
    client,
    state,
    options,
    add(name, ...nodes) {
      const body = bodies.get(name);
      for (const node of nodes) if (node && body) body.appendChild(node);
    },
    draw,
    run,
    leaveAfter,
    confirm: (confirmOptions) => confirmAction(confirmOptions, dialogs.signal),
  };

  function draw(name: SectionName): void {
    const body = bodies.get(name);
    const section = SECTIONS.find((candidate) => candidate.name === name);
    if (!body || !section) return;
    clear(body);
    const error = state.errors.get(name);
    if (error) {
      // Read out once, when it is new, not on every later redraw.
      body.appendChild(statusLine("bad", error, undefined, state.freshErrors.delete(name)));
    }
    section.draw(context);
  }

  async function run(name: SectionName, work: () => Promise<void>): Promise<boolean> {
    if (state.leaving || state.disposed) return false;
    state.busy.add(name);
    state.errors.delete(name);
    draw(name);
    let worked = true;
    try {
      await work();
    } catch (error) {
      worked = false;
      state.errors.set(name, explain(error));
      state.freshErrors.add(name);
    } finally {
      state.busy.delete(name);
      if (!state.leaving && !state.disposed) draw(name);
    }
    return worked;
  }

  async function leaveAfter(
    name: SectionName,
    request: () => Promise<void>,
    message: string,
    willReconnect: boolean,
  ): Promise<void> {
    if (!(await run(name, request))) return;
    state.leaving = true;
    options.onLeaving(message, willReconnect);
  }

  async function load(): Promise<void> {
    try {
      const [{ config, options: choices }, stats, clients] = await Promise.all([
        client.getHubConfig(),
        client.hubStats(),
        state.pinOn ? client.listClients() : Promise.resolve([]),
      ]);
      state.config = readBridgeConfig(config);
      state.choices = readBridgeOptions(choices);
      state.stats = readBridgeStats(stats.bridge);
      state.clients = clients;
      if (!state.config || !state.choices) state.loadError = UNREADABLE_SETTINGS;
    } catch (error) {
      state.loadError = explain(error);
    }
    if (!state.disposed) drawAll();
  }

  function drawAll(): void {
    for (const section of SECTIONS) draw(section.name);
  }

  const root = el(
    "div",
    { class: "settings" },
    el(
      "div",
      { class: "row spread page-head" },
      el("h1", { text: "Bridge settings" }),
      el("button", { type: "button", text: "Done", onClick: () => options.onBack() }),
    ),
    ...SECTIONS.map(({ name, title }) => {
      const body = el("div");
      bodies.set(name, body);
      return el(
        "section",
        { class: "panel", "aria-labelledby": `section-${name}` },
        el("h2", { id: `section-${name}`, text: title }),
        body,
      );
    }),
  );

  const stopConfig = client.on("hubConfig", (config) => {
    // Another admin, or this page, changed something. Sections with unsaved
    // edits keep them; the others show the new values.
    const checked = readBridgeConfig(config);
    if (!checked) return;
    state.config = checked;
    if (!state.uartDraft) draw("dspi");
    if (!state.bridgeDraft) draw("bridge");
  });
  const stopDevices = client.on("devices", () => draw("status"));
  // Each refresh starts only after the last one ends, so slow answers never pile up.
  let statsTimer = setTimeout(function refresh(): void {
    void refreshStats(context).finally(() => {
      if (!state.disposed) statsTimer = setTimeout(refresh, STATS_INTERVAL_MS);
    });
  }, STATS_INTERVAL_MS);

  drawAll();
  void load();

  return {
    root,
    dispose() {
      state.disposed = true;
      clearTimeout(statsTimer);
      stopConfig();
      stopDevices();
      dialogs.abort();
    },
  };
}
