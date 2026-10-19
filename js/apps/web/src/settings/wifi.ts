/*
 * The settings page's Wi-Fi section (bridge spec 10.3): the current network,
 * and moving the bridge to another one with hub.wifi.scan and hub.wifi.join
 * (Link spec 7.11). Joining closes this page's connection, so the page hands
 * over to the control page once the bridge has the request.
 */

import { el } from "../dom.js";
import { namedNetworks, passwordFits, signalWords, WIFI_PASSWORD_MAX_LENGTH } from "../wifi.js";
import type { SectionContext } from "./state.js";

/**
 * Draws the Wi-Fi section from the state.
 *
 * @param context The page's context.
 */
export function drawWifi(context: SectionContext): void {
  const current = context.state.stats?.wifi;
  context.add(
    "wifi",
    current?.ssid
      ? el("p", {}, "Connected to ", el("strong", { text: current.ssid }), ` at ${current.ip}.`)
      : el("p", { class: "dim", text: "Reading the current network…" }),
  );
  switch (context.state.wifiStep) {
    case "idle":
      context.add(
        "wifi",
        el("button", {
          type: "button",
          text: "Change network",
          onClick: () => void scanNetworks(context),
        }),
      );
      return;
    case "scanning":
      context.add(
        "wifi",
        el("p", {
          class: "dim",
          role: "status",
          text: "Looking for networks… this takes a few seconds.",
        }),
      );
      return;
    case "choosing":
      drawChoosing(context);
      return;
    case "password":
      drawPassword(context);
      return;
  }
}

/** The list from the last scan. */
function drawChoosing(context: SectionContext): void {
  const { state } = context;
  const list = el("ul", { class: "network-list" });
  for (const network of state.networks) {
    const choose = (): void => {
      state.joinSsid = network.ssid;
      state.joinSecure = network.secure;
      state.joinPassword = "";
      state.wifiStep = "password";
      context.draw("wifi");
      document.getElementById("wifi-password")?.focus();
    };
    const signal = `${signalWords(network.rssi)} signal${network.secure ? "" : ", open"}`;
    list.appendChild(
      el(
        "li",
        {},
        el(
          "button",
          { type: "button", onClick: choose },
          el("span", { class: "grow", text: network.ssid }),
          el("span", { class: "dim small", text: signal }),
        ),
      ),
    );
  }
  context.add(
    "wifi",
    el("p", { class: "dim small", text: "Only 2.4 GHz networks are shown." }),
    state.networks.length ? list : el("p", { class: "dim", text: "No networks found." }),
    el(
      "div",
      { class: "row" },
      el("button", { type: "button", text: "Refresh", onClick: () => void scanNetworks(context) }),
      el("button", { type: "button", text: "Cancel", onClick: () => cancelWifi(context) }),
    ),
  );
}

/** The chosen network's password, and the Join button. */
function drawPassword(context: SectionContext): void {
  const { state } = context;
  const current = state.stats?.wifi;
  const join = el("button", {
    type: "button",
    class: "primary",
    text: `Join "${state.joinSsid}"`,
    onClick: () => void joinNetwork(context),
  });
  const refresh = (): void => {
    join.disabled = state.busy.has("wifi") || !passwordFits(state.joinPassword, state.joinSecure);
  };
  const field = el("input", {
    type: "password",
    id: "wifi-password",
    value: state.joinPassword,
    autocomplete: "off",
    maxLength: WIFI_PASSWORD_MAX_LENGTH,
    onInput: (event: Event) => {
      state.joinPassword = (event.target as HTMLInputElement).value;
      refresh();
    },
  });
  const fallback = current?.ssid ? `"${current.ssid}"` : "its current network";
  context.add(
    "wifi",
    el(
      "div",
      { class: "field" },
      el("label", { for: "wifi-password", text: `Password for "${state.joinSsid}"` }),
      field,
      state.joinSecure
        ? null
        : el("p", { class: "dim small", text: "This network is open; leave the password empty." }),
    ),
    el("p", {
      class: "dim small",
      text:
        "The bridge leaves its current network to join this one, which closes this page's " +
        "connection. If the join works, the page reconnects once this device is on the same " +
        `network. If it doesn't, the bridge goes back to ${fallback} within about 30 seconds.`,
    }),
    el(
      "div",
      { class: "row" },
      join,
      el("button", { type: "button", text: "Back", onClick: () => backToList(context) }),
    ),
  );
  refresh();
}

function backToList(context: SectionContext): void {
  context.state.wifiStep = "choosing";
  context.state.errors.delete("wifi");
  context.draw("wifi");
}

async function scanNetworks(context: SectionContext): Promise<void> {
  const { client, state } = context;
  state.wifiStep = "scanning";
  const worked = await context.run("wifi", async () => {
    state.networks = namedNetworks((await client.scanWifi()).networks);
  });
  state.wifiStep = worked ? "choosing" : "idle";
  context.draw("wifi");
}

function cancelWifi(context: SectionContext): void {
  context.state.wifiStep = "idle";
  context.state.errors.delete("wifi");
  context.draw("wifi");
}

async function joinNetwork(context: SectionContext): Promise<void> {
  const { client, state } = context;
  const ssid = state.joinSsid;
  const previous = state.stats?.wifi.ssid;
  const fallback = previous
    ? ` If the bridge can't join, it goes back to "${previous}" within about 30 seconds.`
    : "";
  await context.leaveAfter(
    "wifi",
    () => client.joinWifi(ssid, state.joinPassword),
    `The bridge is joining "${ssid}". This page reconnects once this device is on the same ` +
      `network.${fallback}`,
    true,
  );
}
