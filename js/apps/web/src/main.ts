/*
 * The control page every hub serves from / (bridge spec 10.1; README "The web
 * app").
 *
 * Four screens: connect (find a hub and authenticate, Link spec 3.3 and 6),
 * the device list (7.4), one device's volume and meters (device/page.ts), and
 * for a bridge its settings (settings/page.ts). The page defaults to the
 * origin that served it, which is the common case: a hub serving this bundle
 * from /. This file owns the session and decides which screen shows.
 */

import {
  LINK_DEFAULT_PORT,
  LinkClient,
  LinkProtocolError,
  type DeviceInfo,
  type HubInfo,
  type LinkCloseInfo,
  type LinkRole,
  fetchHubInfo,
  pinSourceOf,
  webSocketUrl,
} from "@dspi/link";
import { append, clear, el } from "./dom.js";
import { createDevicePage, type DevicePageHandles } from "./device/page.js";
import { describeProblem } from "./problems.js";
import { createSettingsPage, type SettingsPageHandles } from "./settings/page.js";
import {
  forgetToken,
  loadClientName,
  loadHost,
  loadToken,
  saveClientName,
  saveHost,
  saveToken,
} from "./storage.js";
import "./style.css";

/** States in which a session exists, so Connect would replace it. */
const SESSION_STATES = new Set([
  "connecting",
  "handshaking",
  "authenticating",
  "awaiting_pairing",
  "reconnecting",
]);

/** The roles a window-PIN hub can grant, in words (Link spec 6.3). */
const ROLE_CHOICES: Array<{ role: LinkRole; label: string }> = [
  { role: "control", label: "Control: change the DSPi's settings" },
  { role: "viewer", label: "Viewer: watch only" },
  { role: "admin", label: "Admin: everything, including the hub's own settings" },
];

/** Where to look for a hub before the user types anything. */
function defaultHost(): string {
  const stored = loadHost();
  if (stored) return stored;
  if (location.protocol.startsWith("http") && location.host) return location.host;
  return `localhost:${LINK_DEFAULT_PORT}`;
}

const pageIsSecure = location.protocol === "https:";

/** The first pattern that matches the user agent, or null. */
function firstMatch(agent: string, names: Array<[RegExp, string]>): string | null {
  return names.find(([pattern]) => pattern.test(agent))?.[1] ?? null;
}

/**
 * A name for the hub's list of paired clients that tells this browser apart
 * from others: "Web UI, Safari on iPhone". Only a guess from the user agent;
 * the user can type any name before connecting.
 */
function defaultClientName(): string {
  const agent = navigator.userAgent;
  const browser = firstMatch(agent, [
    [/Edg\//, "Edge"],
    [/Firefox\//, "Firefox"],
    [/Chrome\//, "Chrome"],
    [/Safari\//, "Safari"],
  ]);
  const device = firstMatch(agent, [
    [/iPhone/, "iPhone"],
    [/iPad/, "iPad"],
    [/Android/, "Android"],
    [/Mac OS X/, "Mac"],
    [/Windows/, "Windows"],
    [/Linux/, "Linux"],
  ]);
  if (browser && device) return `Web UI, ${browser} on ${device}`;
  return "Web UI";
}

/** What to tell the user when pairing fails. */
function pairingProblem(error: unknown): string {
  if (error instanceof LinkProtocolError) {
    if (error.code === "unauthenticated" || error.code === "denied") return "That PIN isn't right.";
    if (error.code === "rate_limited") return "Too many wrong PINs. Wait a minute and try again.";
  }
  return describeProblem(error);
}

/** Why a session ended for good, in words, or null when nothing needs saying. */
function closeProblem(info: LinkCloseInfo): string | null {
  if (info.willReconnect) return null;
  switch (info.reason) {
    case "normal":
      return null;
    case "auth_failed":
      return "The hub closed the connection: too many wrong PINs, or none in time. Wait a minute and connect again.";
    case "revoked":
      return "This browser was removed from the hub's paired list. Connect and enter the PIN again.";
    case "protocol_error":
      return "This page and the hub don't understand each other. Update the hub's firmware, then reload this page.";
    default:
      return "The connection to the hub was lost.";
  }
}

const app = document.getElementById("app")!;
const client = new LinkClient({ reconnect: true });

let host = defaultHost();
let info: HubInfo | null = null;
let infoError: string | null = null;
let connectError: string | null = null;
let pairError: string | null = null;
let busy = false;
let clientName = loadClientName() ?? defaultClientName();
let selectedHandle: number | null = null;
let devicePage: DevicePageHandles | null = null;
/** True while the bridge settings are open; kept across a reconnect, so a
 * restart from the settings page comes back to them. */
let settingsOpen = false;
let settingsPage: SettingsPageHandles | null = null;
/** Why the connection went away on purpose, shown on the connect screen. */
let notice: string | null = null;
/** The firmware a bridge was told to restart into, until a hello shows
 * whether it did (Link spec 7.14). */
let expectedFirmware: string | null = null;

// ---------------------------------------------------------------------------
// Rendering

function render(): void {
  // A live device page keeps its poll subscription and its meter DOM; tearing
  // it down and rebuilding it on every unrelated event would resubscribe at
  // 10 Hz and flicker the bars.
  const ready = client.state === "ready";
  if (ready && selectedHandle !== null && devicePage?.handle === selectedHandle) return;
  // The settings page keeps half-typed values and its own refresh timer.
  if (ready && settingsOpen && settingsPage) return;
  closeDevicePage();
  closeSettingsPage();
  clear(app);
  if (!ready) renderConnect();
  else if (settingsOpen && canOpenSettings()) renderSettings();
  else if (selectedHandle !== null) renderDevice(selectedHandle);
  else renderDeviceList();
}

async function probe(hostField: HTMLInputElement): Promise<void> {
  host = hostField.value.trim() || defaultHost();
  saveHost(host);
  info = null;
  infoError = null;
  busy = true;
  render();
  try {
    info = await fetchHubInfo(host, pageIsSecure);
  } catch (error) {
    infoError = describeProblem(error);
  } finally {
    busy = false;
    render();
  }
}

async function connect(nameField: HTMLInputElement): Promise<void> {
  clientName = nameField.value.trim() || defaultClientName();
  saveClientName(clientName);
  connectError = null;
  notice = null;
  busy = true;
  render();
  const hubId = info?.hub.id;
  const token = hubId ? (loadToken(hubId) ?? undefined) : undefined;
  try {
    await client.connect(webSocketUrl(host, info ?? undefined, pageIsSecure), {
      clientName,
      token,
      app: "DSPi Web",
      version: __APP_VERSION__,
    });
    // The client drops a token only when the hub refused it.
    if (token && hubId && client.state === "awaiting_pairing" && client.token === null) {
      forgetToken(hubId);
    }
  } catch (error) {
    connectError = describeProblem(error);
  } finally {
    busy = false;
    render();
  }
}

/** A text field on the connect screen; its <label> points at `id`. */
function connectField(id: string, value: string, helpId?: string): HTMLInputElement {
  return el("input", {
    type: "text",
    id,
    class: "grow",
    value,
    autocapitalize: "off",
    "aria-describedby": helpId ?? null,
  }) as HTMLInputElement;
}

function hubPanel(): HTMLElement {
  const panel = el("div", { class: "panel", role: "status" });
  if (busy && !info) {
    panel.appendChild(el("p", { class: "dim", text: "Looking for a hub…" }));
    return panel;
  }
  if (!info) {
    panel.append(
      el("h1", { text: "DSPi" }),
      el("p", { class: infoError ? "error" : "dim", text: infoError ?? "No hub found yet." }),
    );
    return panel;
  }
  const facts = [info.hub.kind, info.hub.version ? `version ${info.hub.version}` : null]
    .filter(Boolean)
    .join(", ");
  const shared = info.devices?.length
    ? `${info.devices.length} DSPi${info.devices.length === 1 ? "" : "s"} shared`
    : null;
  append(panel, [
    el("h1", { text: info.hub.name }),
    el("p", { class: "dim", text: facts }),
    el("p", {
      class: "dim",
      text: info.auth === "pin" ? "Needs a PIN" : "Open to everyone on this network",
    }),
    shared ? el("p", { class: "dim", text: shared }) : null,
  ]);
  return panel;
}

function connectPanel(): HTMLElement {
  const hostField = connectField("hub-address", host);
  hostField.placeholder = `name.local or name.local:${LINK_DEFAULT_PORT}`;
  const nameField = connectField("client-name", clientName, "client-name-help");
  const sessionOpen = SESSION_STATES.has(client.state);
  const action = sessionOpen
    ? el("button", { type: "button", text: "Stop", onClick: disconnect })
    : el("button", {
        type: "button",
        class: "primary",
        text: "Connect",
        disabled: busy,
        onClick: () => void connect(nameField),
      });
  return el(
    "div",
    { class: "panel" },
    el("label", { for: "hub-address", class: "field-label", text: "Hub address" }),
    el(
      "div",
      { class: "row" },
      hostField,
      el("button", {
        type: "button",
        text: "Find",
        disabled: busy || sessionOpen,
        onClick: () => void probe(hostField),
      }),
    ),
    el("label", { for: "client-name", class: "field-label spaced", text: "Name this browser" }),
    el("div", { class: "row" }, nameField, action),
    el("p", {
      class: "dim small",
      id: "client-name-help",
      text: "The hub lists paired browsers by this name.",
    }),
    connectError ? el("p", { class: "error", role: "alert", text: connectError }) : null,
  );
}

function renderConnect(): void {
  if (notice) {
    app.appendChild(
      el("div", { class: "panel notice", role: "status" }, el("p", { text: notice })),
    );
  }
  app.append(hubPanel(), connectPanel());
  if (client.state === "awaiting_pairing") app.appendChild(renderPairing());
  if (client.state === "reconnecting") {
    app.appendChild(
      el("p", { class: "dim", role: "status", text: "Connection lost, trying again…" }),
    );
  }
}

/** The role picker for a window-PIN hub. */
function roleField(): HTMLSelectElement {
  return el(
    "select",
    { id: "pair-role" },
    ...ROLE_CHOICES.map(({ role, label }) => el("option", { value: role, text: label })),
  ) as HTMLSelectElement;
}

/**
 * The PIN prompt. A hub with a fixed PIN (a bridge) is asked for the PIN the
 * user chose; any other hub shows a PIN for a moment in its pairing window
 * (Link spec 6.2). A fixed-PIN hub grants every client the same role, so the
 * role choice is only offered for window PINs.
 */
function renderPairing(): HTMLElement {
  const fixed = client.hub ? pinSourceOf(client.hub) === "fixed" : false;
  const hubWord = client.hub?.hub.kind === "bridge" ? "bridge" : "hub";
  const pinField = el("input", {
    type: fixed ? "password" : "text",
    id: "pair-pin",
    inputmode: "numeric",
    autocomplete: "off",
    class: "grow",
    maxlength: "6",
  }) as HTMLInputElement;
  const role = fixed ? null : roleField();

  const submit = async (): Promise<void> => {
    pairError = null;
    busy = true;
    render();
    try {
      const chosen = role ? (role.value as LinkRole) : undefined;
      const token = await client.pair(pinField.value.trim(), clientName, chosen);
      const hubId = client.hubId ?? info?.hub.id;
      if (token && hubId) saveToken(hubId, token);
    } catch (error) {
      pairError = pairingProblem(error);
    } finally {
      busy = false;
      render();
    }
  };
  pinField.addEventListener("keydown", (event) => {
    if (event.key === "Enter") void submit();
  });
  setTimeout(() => pinField.focus(), 0);

  const prompt = fixed
    ? `This ${hubWord} is protected with a PIN. Enter the ${hubWord}'s PIN; this browser remembers it after that.`
    : "This hub needs a PIN. Open its pairing window and enter the PIN shown on the hub.";
  return el(
    "div",
    { class: "panel" },
    el("h2", { text: fixed ? `Enter the ${hubWord}'s PIN` : "Pairing" }),
    el("p", { class: "dim", text: prompt }),
    el("label", { for: "pair-pin", class: "field-label", text: "PIN (6 digits)" }),
    el(
      "div",
      { class: "row" },
      pinField,
      el("button", {
        type: "button",
        class: "primary",
        text: "Pair",
        disabled: busy,
        onClick: () => void submit(),
      }),
    ),
    role
      ? el("label", { for: "pair-role", class: "field-label spaced", text: "Access to ask for" })
      : null,
    role,
    pairError ? el("p", { class: "error", role: "alert", text: pairError }) : null,
  );
}

function renderDeviceList(): void {
  const devices = client.deviceList;
  app.appendChild(
    el(
      "div",
      { class: "row spread", style: "margin-bottom:16px" },
      el(
        "div",
        {},
        el("h1", { text: info?.hub.name ?? "Hub", style: "margin:0" }),
        el("span", { class: "dim mono", text: host }),
      ),
      el(
        "div",
        { class: "row" },
        el("span", { class: "badge", text: client.role ? `Role: ${client.role}` : "" }),
        canOpenSettings()
          ? el("button", { type: "button", text: "Bridge settings", onClick: openSettings })
          : null,
        el("button", { type: "button", text: "Disconnect", onClick: disconnect }),
      ),
    ),
  );
  const panel = el("div", { class: "panel" }, el("h2", { text: "Devices" }));
  if (devices.length === 0) {
    panel.appendChild(el("p", { class: "dim", text: "This hub is not sharing any DSPi." }));
  }
  for (const device of devices) panel.appendChild(deviceRow(device));
  app.appendChild(panel);
}

function openSettings(): void {
  settingsOpen = true;
  render();
}

function deviceRow(device: DeviceInfo): HTMLElement {
  return el(
    "button",
    {
      type: "button",
      class: "device-item",
      disabled: device.state !== "online",
      onClick: () => {
        selectedHandle = device.handle;
        render();
      },
    },
    el(
      "span",
      {},
      el("span", { class: "name", text: device.name }),
      el("br"),
      el("span", { class: "dim mono", text: device.serial }),
    ),
    el("span", { class: `badge ${device.state}`, text: device.state }),
  );
}

function renderDevice(handle: number): void {
  const device = client.device(handle);
  if (!device) {
    selectedHandle = null;
    renderDeviceList();
    return;
  }
  devicePage = createDevicePage(client, device, () => {
    selectedHandle = null;
    render();
  });
  app.appendChild(devicePage.root);
}

function closeDevicePage(): void {
  devicePage?.dispose();
  devicePage = null;
}

/** The settings page is for hubs that can be administered over Link, and
 * only an admin session may do it (Link spec 7.9). */
function canOpenSettings(): boolean {
  return client.hasCap("hub_config") && client.role === "admin";
}

/** How a firmware update went, judged from the hello after it. */
function firmwareOutcome(): { tone: "good" | "bad"; text: string } | undefined {
  if (!expectedFirmware) return undefined;
  const running = client.hub?.hub.version ?? "";
  const expected = expectedFirmware;
  expectedFirmware = null;
  if (running === expected) return { tone: "good", text: `Firmware ${expected} is installed.` };
  return {
    tone: "bad",
    text: `The bridge is running firmware ${running}: firmware ${expected} did not start properly, so the bridge went back.`,
  };
}

function renderSettings(): void {
  settingsPage = createSettingsPage(client, {
    announce: firmwareOutcome(),
    onFirmwareInstalled: (version) => {
      expectedFirmware = version;
    },
    onBack: () => {
      settingsOpen = false;
      render();
    },
    onToken: (token) => {
      const hubId = client.hubId;
      if (!hubId) return;
      if (token) saveToken(hubId, token);
      else forgetToken(hubId);
    },
    onLeaving: (message, willReconnect) => {
      notice = message;
      if (willReconnect) return;
      // The bridge is leaving this network for good; stop trying to reach it.
      settingsOpen = false;
      client.disconnect();
      render();
    },
  });
  app.appendChild(settingsPage.root);
}

function closeSettingsPage(): void {
  settingsPage?.dispose();
  settingsPage = null;
}

function disconnect(): void {
  closeDevicePage();
  closeSettingsPage();
  settingsOpen = false;
  selectedHandle = null;
  client.disconnect();
  render();
}

// ---------------------------------------------------------------------------
// Wiring

client.on("state", ({ state }) => {
  if (state === "ready") notice = null;
  render();
});
client.on("hubConfig", (config) => {
  // Keep the header's name current after a rename from the settings page.
  if (info && typeof config.name === "string") info.hub.name = config.name;
});
client.on("devices", () => {
  // The device page manages itself; the list, and a device that went away
  // while we were looking at it, need redrawing from here.
  if (selectedHandle !== null && !client.device(selectedHandle)) selectedHandle = null;
  if (selectedHandle === null) render();
});
client.on("close", (close) => {
  if (close.reason === "revoked" && client.hubId) forgetToken(client.hubId);
  const problem = closeProblem(close);
  if (problem) connectError = problem;
});
client.on("error", (error) => console.warn("link:", error.message));

render();
void (async () => {
  try {
    info = await fetchHubInfo(host, pageIsSecure);
  } catch (error) {
    infoError = describeProblem(error);
  }
  render();
})();
