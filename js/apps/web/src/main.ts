/*
 * DSPi web UI, v1.
 *
 * Three screens: connect (find and authenticate to a hub), the device list,
 * and one device's master volume and meters. The page defaults to the origin
 * that served it, which is the common case: a hub serving this bundle from /.
 */

import {
  LinkClient,
  type DeviceInfo,
  type HubInfo,
  fetchHubInfo,
  webSocketUrl,
} from "@dspi/link";
import { clear, el } from "./dom.js";
import { createDevicePage, type DevicePageHandles } from "./device.js";
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

const app = document.getElementById("app")!;
const client = new LinkClient({ reconnect: true });

let host = defaultHost();
let info: HubInfo | null = null;
let infoError: string | null = null;
let connectError: string | null = null;
let pairError: string | null = null;
let busy = false;
let clientName = readStorage(STORAGE_NAME) ?? "Web UI";
let selectedHandle: number | null = null;
let devicePage: DevicePageHandles | null = null;

// ---------------------------------------------------------------------------
// Rendering

function render(): void {
  // A live device page keeps its poll subscription and its meter DOM; tearing
  // it down and rebuilding it on every unrelated event would resubscribe at
  // 10 Hz and flicker the bars.
  if (client.state === "ready" && selectedHandle !== null && devicePage?.handle === selectedHandle) {
    return;
  }
  closeDevicePage();
  clear(app);
  if (client.state === "ready") {
    if (selectedHandle !== null) renderDevice(selectedHandle);
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
    clientName = nameField.value.trim() || "Web UI";
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
        version: "1.0.0",
      });
    } catch (e) {
      connectError = (e as Error).message;
    } finally {
      busy = false;
      render();
    }
  };

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

function renderPairing(): HTMLElement {
  const pinField = el("input", {
    type: "text",
    inputmode: "numeric",
    class: "grow",
    placeholder: "6-digit PIN",
    maxlength: "6",
  }) as HTMLInputElement;

  const roleField = el(
    "select",
    {},
    el("option", { value: "control", text: "Control" }),
    el("option", { value: "viewer", text: "Viewer" }),
    el("option", { value: "admin", text: "Admin" }),
  ) as HTMLSelectElement;

  const submit = async (): Promise<void> => {
    pairError = null;
    busy = true;
    render();
    try {
      const token = await client.pair(
        pinField.value.trim(),
        clientName,
        roleField.value as "viewer" | "control" | "admin",
      );
      const hubId = client.hubId ?? info?.hub.id;
      if (token && hubId) writeStorage(tokenKey(hubId), token);
    } catch (e) {
      pairError = (e as Error).message;
    } finally {
      busy = false;
      render();
    }
  };

  const panel = el(
    "div",
    { class: "panel" },
    el("h2", { text: "Pairing" }),
    el("p", {
      class: "dim",
      text: "This hub needs a PIN. Open its pairing window and type the number it shows.",
    }),
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

function disconnect(): void {
  closeDevicePage();
  selectedHandle = null;
  client.disconnect();
  render();
}

// ---------------------------------------------------------------------------
// Wiring

client.on("state", () => render());
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
