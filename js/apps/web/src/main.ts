/*
 * DSPi web UI, v1.
 *
 * Four screens: connect (find and authenticate to a hub), the device list,
 * one device's master volume and meters, and, for a bridge, its settings
 * (settings.ts). The page defaults to the origin that served it, which is the
 * common case: a hub serving this bundle from /.
 */

import {
  LinkClient,
  LinkProtocolError,
  type DeviceInfo,
  type HubInfo,
  fetchHubInfo,
  pinSourceOf,
  webSocketUrl,
} from "@dspi/link";
import { clear, el } from "./dom.js";
import { createDevicePage, type DevicePageHandles } from "./device.js";
import { createSettingsPage, type SettingsPageHandles } from "./settings.js";
import "./style.css";

const STORAGE_HOST = "dspi-link.host";
const STORAGE_NAME = "dspi-link.client_name";
/** Tokens are keyed on the hub id, never the address, which changes (spec 6.2). */
const tokenKey = (hubId: string) => `dspi-link.token.${hubId}`;

function readStorage(key: string): string | null {
  try {
    return localStorage.getItem(key);
  } catch {
    return null; // Private mode or blocked storage: work without persistence.
  }
}

function writeStorage(key: string, value: string): void {
  try {
    localStorage.setItem(key, value);
  } catch {
    // Nothing to do; the session still works, it just will not be remembered.
  }
}

function removeStorage(key: string): void {
  try {
    localStorage.removeItem(key);
  } catch {
    // Same as above.
  }
}

/** Where to look for a hub before the user types anything. */
function defaultHost(): string {
  const stored = readStorage(STORAGE_HOST);
  if (stored) return stored;
  if (location.protocol.startsWith("http") && location.host) return location.host;
  return "localhost:11915";
}

const pageIsSecure = location.protocol === "https:";

/**
 * A name for the hub's list of paired clients that tells this browser apart
 * from others: "Web UI, Safari on iPhone". Only a guess from the user agent;
 * the user can type any name before connecting.
 */
function defaultClientName(): string {
  const agent = navigator.userAgent;
  const browser = /Edg\//.test(agent)
    ? "Edge"
    : /Firefox\//.test(agent)
      ? "Firefox"
      : /Chrome\//.test(agent)
        ? "Chrome"
        : /Safari\//.test(agent)
          ? "Safari"
          : null;
  const device = /iPhone/.test(agent)
    ? "iPhone"
    : /iPad/.test(agent)
      ? "iPad"
      : /Android/.test(agent)
        ? "Android"
        : /Mac OS X/.test(agent)
          ? "Mac"
          : /Windows/.test(agent)
            ? "Windows"
            : /Linux/.test(agent)
              ? "Linux"
              : null;
  if (browser && device) return `Web UI, ${browser} on ${device}`;
  return "Web UI";
}

/** What to tell the user when pairing fails. */
function pairingProblem(error: unknown): string {
  if (error instanceof LinkProtocolError) {
    if (error.code === "unauthenticated" || error.code === "denied") return "That PIN isn't right.";
    if (error.code === "rate_limited") return "Too many wrong PINs. Wait a minute and try again.";
  }
  return (error as Error).message;
}

const app = document.getElementById("app")!;
const client = new LinkClient({ reconnect: true });

let host = defaultHost();
let info: HubInfo | null = null;
let infoError: string | null = null;
let connectError: string | null = null;
let pairError: string | null = null;
let busy = false;
let clientName = readStorage(STORAGE_NAME) ?? defaultClientName();
let selectedHandle: number | null = null;
let devicePage: DevicePageHandles | null = null;
/** True while the bridge settings are open; kept across a reconnect, so a
 * restart from the settings page comes back to them. */
let settingsOpen = false;
let settingsPage: SettingsPageHandles | null = null;
/** Why the connection went away on purpose, shown on the connect screen. */
let notice: string | null = null;

// ---------------------------------------------------------------------------
// Rendering

function render(): void {
  // A live device page keeps its poll subscription and its meter DOM; tearing
  // it down and rebuilding it on every unrelated event would resubscribe at
  // 10 Hz and flicker the bars.
  if (
    client.state === "ready" &&
    selectedHandle !== null &&
    devicePage?.handle === selectedHandle
  ) {
    return;
  }
  // The settings page keeps half-typed values and its own refresh timer.
  if (client.state === "ready" && settingsOpen && settingsPage) {
    return;
  }
  closeDevicePage();
  closeSettingsPage();
  clear(app);
  if (client.state === "ready") {
    if (settingsOpen && canOpenSettings()) renderSettings();
    else if (selectedHandle !== null) renderDevice(selectedHandle);
    else renderDeviceList();
  } else {
    renderConnect();
  }
}

function renderConnect(): void {
  const hostField = el("input", {
    type: "text",
    class: "grow",
    value: host,
    placeholder: "host or host:port",
    autocapitalize: "off",
  }) as HTMLInputElement;

  const nameField = el("input", {
    type: "text",
    class: "grow",
    value: clientName,
    placeholder: "This client's name",
  }) as HTMLInputElement;

  const probe = async (): Promise<void> => {
    host = hostField.value.trim() || defaultHost();
    writeStorage(STORAGE_HOST, host);
    info = null;
    infoError = null;
    busy = true;
    render();
    try {
      info = await fetchHubInfo(host, pageIsSecure);
    } catch (e) {
      infoError = (e as Error).message;
    } finally {
      busy = false;
      render();
    }
  };

  const connect = async (): Promise<void> => {
    clientName = nameField.value.trim() || defaultClientName();
    writeStorage(STORAGE_NAME, clientName);
    connectError = null;
    busy = true;
    render();
    try {
      const hubId = info?.hub.id;
      const token = hubId ? (readStorage(tokenKey(hubId)) ?? undefined) : undefined;
      await client.connect(webSocketUrl(host, info ?? undefined, pageIsSecure), {
        clientName,
        token,
        app: "DSPi Web",
        version: "1.2.0",
      });
    } catch (e) {
      connectError = (e as Error).message;
    } finally {
      busy = false;
      render();
    }
  };

  if (notice) {
    app.appendChild(
      el("div", { class: "panel notice", role: "status" }, el("p", { text: notice })),
    );
  }

  const hubPanel = el("div", { class: "panel" });
  if (busy && !info) {
    hubPanel.appendChild(el("p", { class: "dim", text: "Looking for a hub..." }));
  } else if (info) {
    hubPanel.appendChild(el("h1", { text: info.hub.name }));
    const facts = [info.hub.kind, info.hub.version ? `v${info.hub.version}` : null]
      .filter(Boolean)
      .join(", ");
    hubPanel.appendChild(el("p", { class: "dim", text: facts }));
    hubPanel.appendChild(
      el("p", { class: "dim", text: info.auth === "pin" ? "Pairing required" : "Open access" }),
    );
    if (info.devices?.length) {
      hubPanel.appendChild(
        el("p", {
          class: "dim",
          text: `${info.devices.length} device${info.devices.length === 1 ? "" : "s"} shared`,
        }),
      );
    }
  } else {
    hubPanel.appendChild(el("h1", { text: "DSPi" }));
    hubPanel.appendChild(
      el("p", { class: infoError ? "error" : "dim", text: infoError ?? "No hub found yet." }),
    );
  }

  app.appendChild(hubPanel);

  app.appendChild(
    el(
      "div",
      { class: "panel" },
      el("h2", { text: "Hub" }),
      el(
        "div",
        { class: "row" },
        hostField,
        el("button", { type: "button", text: "Find", disabled: busy, onClick: () => void probe() }),
      ),
      el("h2", { text: "Connect as", style: "margin-top:16px" }),
      el(
        "div",
        { class: "row" },
        nameField,
        el("button", {
          type: "button",
          class: "primary",
          text: "Connect",
          disabled: busy,
          onClick: () => void connect(),
        }),
      ),
      connectError ? el("p", { class: "error", text: connectError }) : null,
    ),
  );

  if (client.state === "awaiting_pairing") app.appendChild(renderPairing());
  if (client.state === "reconnecting") {
    app.appendChild(el("p", { class: "dim", text: "Connection lost, retrying..." }));
  }
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
    inputmode: "numeric",
    autocomplete: "off",
    class: "grow",
    placeholder: "6-digit PIN",
    maxlength: "6",
  }) as HTMLInputElement;

  const roleField = fixed
    ? null
    : (el(
        "select",
        { "aria-label": "Role" },
        el("option", { value: "control", text: "Control" }),
        el("option", { value: "viewer", text: "Viewer" }),
        el("option", { value: "admin", text: "Admin" }),
      ) as HTMLSelectElement);

  const submit = async (): Promise<void> => {
    pairError = null;
    busy = true;
    render();
    try {
      const role = roleField ? (roleField.value as "viewer" | "control" | "admin") : undefined;
      const token = await client.pair(pinField.value.trim(), clientName, role);
      const hubId = client.hubId ?? info?.hub.id;
      if (token && hubId) writeStorage(tokenKey(hubId), token);
    } catch (e) {
      pairError = pairingProblem(e);
    } finally {
      busy = false;
      render();
    }
  };

  const prompt = fixed
    ? `This ${hubWord} is protected with a PIN. Enter the ${hubWord}'s PIN; this browser remembers it after that.`
    : "This hub needs a PIN. Open its pairing window and enter the PIN shown on the hub.";

  const panel = el(
    "div",
    { class: "panel" },
    el("h2", { text: fixed ? `Enter the ${hubWord}'s PIN` : "Pairing" }),
    el("p", { class: "dim", text: prompt }),
    el(
      "div",
      { class: "row" },
      pinField,
      roleField,
      el("button", {
        type: "button",
        class: "primary",
        text: "Pair",
        disabled: busy,
        onClick: () => void submit(),
      }),
    ),
    pairError ? el("p", { class: "error", text: pairError }) : null,
  );
  pinField.addEventListener("keydown", (event) => {
    if (event.key === "Enter") void submit();
  });
  setTimeout(() => pinField.focus(), 0);
  return panel;
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
        el("span", { class: "badge", text: client.role ?? "" }),
        canOpenSettings()
          ? el("button", {
              type: "button",
              text: "Bridge settings",
              onClick: () => {
                settingsOpen = true;
                render();
              },
            })
          : null,
        el("button", { type: "button", text: "Disconnect", onClick: disconnect }),
      ),
    ),
  );

  const panel = el("div", { class: "panel" }, el("h2", { text: "Devices" }));
  if (devices.length === 0) {
    panel.appendChild(el("p", { class: "dim", text: "This hub is not sharing any device." }));
  }
  for (const device of devices) {
    panel.appendChild(deviceRow(device));
  }
  app.appendChild(panel);
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

function renderSettings(): void {
  settingsPage = createSettingsPage(client, {
    onBack: () => {
      settingsOpen = false;
      render();
    },
    onToken: (token) => {
      const hubId = client.hubId;
      if (!hubId) return;
      if (token) writeStorage(tokenKey(hubId), token);
      else removeStorage(tokenKey(hubId));
    },
    onLeaving: (message, willReconnect) => {
      notice = message;
      if (!willReconnect) {
        // The bridge is leaving this network for good; stop trying to reach it.
        settingsOpen = false;
        client.disconnect();
        render();
      }
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
client.on("close", (c) => {
  if (c.reason === "revoked" && client.hubId) removeStorage(tokenKey(client.hubId));
  if (!c.willReconnect && c.reason !== "normal") {
    connectError = `Connection closed (${c.code}, ${c.reason})`;
  }
});
client.on("error", (e) => console.warn("link:", e.message));

render();
void (async () => {
  try {
    info = await fetchHubInfo(host, pageIsSecure);
  } catch (e) {
    infoError = (e as Error).message;
  }
  render();
})();
