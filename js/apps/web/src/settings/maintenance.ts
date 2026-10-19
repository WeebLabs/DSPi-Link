/*
 * The settings page's Maintenance section (bridge spec 10.3): update the
 * firmware (Link spec 7.14, bridge spec 14.2), restart, forget Wi-Fi and
 * factory reset (Link spec 7.13). Each ends this page's session, so each hands
 * over to the control page; the two that erase something are red and
 * confirmed (bridge spec 10.4).
 */

import { el } from "../dom.js";
import { chipLabel, readFirmwareInfo, type FirmwareInfo } from "../firmware.js";
import { maintenanceRow } from "./fields.js";
import type { SectionContext } from "./state.js";

/** The project name ESP-IDF stamps into bridge firmware. */
const BRIDGE_PROJECT = "dspi_bridge";

const SETUP_AGAIN =
  'To set it up again, join the Wi-Fi network whose name starts with "DSPi Bridge" from a phone.';

/**
 * Draws the Maintenance section from the state.
 *
 * @param context The page's context.
 */
export function drawMaintenance(context: SectionContext): void {
  const busy = context.state.busy.has("maintenance");
  const button = (text: string, onClick: () => void, danger = false): HTMLElement =>
    el("button", {
      type: "button",
      class: danger ? "danger" : null,
      text,
      disabled: busy,
      onClick,
    });
  context.add(
    "maintenance",
    context.client.hasCap("hub_update") ? updateRow(context, busy) : null,
    maintenanceRow(
      "Restart",
      "Restarts the bridge. Everything is kept, and this page reconnects in about 10 seconds.",
      button("Restart", () => void restart(context)),
    ),
    maintenanceRow(
      "Forget Wi-Fi",
      "Forgets the Wi-Fi network, so the bridge can be set up on another one. Everything else is kept.",
      button("Forget Wi-Fi", () => void forgetWifi(context), true),
    ),
    maintenanceRow(
      "Factory reset",
      "Erases every setting on the bridge. Your DSPi's own settings are not touched.",
      button("Factory reset", () => void factoryReset(context), true),
    ),
  );
}

/** "Update firmware": a file picker, then progress while it uploads. */
function updateRow(context: SectionContext, busy: boolean): HTMLElement {
  const progress = context.state.updateProgress;
  if (progress !== null) {
    const percent = Math.round(progress * 100);
    return el(
      "div",
      { class: "maintenance-row" },
      el(
        "div",
        { class: "grow" },
        el("strong", { text: "Updating firmware" }),
        el("p", {
          class: "dim small",
          role: "status",
          text:
            percent < 100
              ? `Sending the firmware: ${percent}%. Keep this page open.`
              : "Checking the firmware…",
        }),
        el("progress", { max: 100, value: percent, "aria-label": "Update progress" }),
      ),
    );
  }
  const picker = el("input", {
    type: "file",
    accept: ".bin,application/octet-stream",
    hidden: true,
    onChange: (event: Event) => {
      const input = event.target as HTMLInputElement;
      const file = input.files?.[0];
      // Cleared so choosing the same file again, after a refusal, is noticed.
      input.value = "";
      if (file) void installFirmware(context, file);
    },
  });
  return maintenanceRow(
    "Update firmware",
    "Installs a new version of the bridge firmware from a .bin file. Everything else is kept.",
    el(
      "div",
      {},
      picker,
      el("button", {
        type: "button",
        text: "Choose file…",
        disabled: busy,
        onClick: () => picker.click(),
      }),
    ),
  );
}

/** Why `info` cannot go on this bridge, or null if it can. */
function firmwareProblem(context: SectionContext, info: FirmwareInfo | null): string | null {
  if (!info) {
    return "That file is not ESP32 firmware. Choose the .bin file made for the DSPi bridge.";
  }
  if (info.project !== BRIDGE_PROJECT) {
    return `That file is firmware for something else ("${info.project}"), not the DSPi bridge.`;
  }
  const chip = context.state.stats?.chip;
  if (chip && info.chip !== chip) {
    return `That file is for the ${chipLabel(info.chip)}, but this bridge is an ${chipLabel(chip)}.`;
  }
  return null;
}

function showProblem(context: SectionContext, problem: string): void {
  context.state.errors.set("maintenance", problem);
  context.state.freshErrors.add("maintenance");
  context.draw("maintenance");
}

async function readImage(file: File): Promise<Uint8Array | null> {
  try {
    return new Uint8Array(await file.arrayBuffer());
  } catch (error) {
    console.warn("settings: could not read the firmware file:", error);
    return null;
  }
}

async function installFirmware(context: SectionContext, file: File): Promise<void> {
  const { client, state, options } = context;
  const image = await readImage(file);
  if (!image) {
    showProblem(context, "That file couldn't be read. Choose it again.");
    return;
  }
  const info = readFirmwareInfo(image);
  const problem = firmwareProblem(context, info);
  if (problem || !info) {
    showProblem(context, problem ?? "That file is not ESP32 firmware.");
    return;
  }
  if (!(await confirmInstall(context, info.version))) return;

  state.updateProgress = 0;
  let installed = "";
  const worked = await context.run("maintenance", async () => {
    installed = await client.updateHub(image, {
      version: info.version,
      onProgress: (written, total) => {
        state.updateProgress = written / total;
        if (!state.disposed) context.draw("maintenance");
      },
    });
  });
  state.updateProgress = null;
  if (!worked) {
    if (!state.disposed) context.draw("maintenance");
    return;
  }
  state.leaving = true;
  options.onFirmwareInstalled(installed);
  options.onLeaving(
    `The bridge is restarting into firmware ${installed}. This page reconnects when it's ` +
      "back, in about 15 seconds.",
    true,
  );
}

function confirmInstall(context: SectionContext, version: string): Promise<boolean> {
  const current = context.client.hub?.hub.version;
  return context.confirm({
    title: `Install firmware ${version}?`,
    body: [
      `${current ? `The bridge runs firmware ${current} now. ` : ""}Installing takes about a ` +
        "minute, and the bridge restarts at the end, which disconnects every app for a moment.",
      "Everything else is kept: Wi-Fi, settings, the PIN and paired browsers and apps.",
      "If the new firmware does not start properly, the bridge goes back to this one by itself.",
    ],
    confirmLabel: "Install",
  });
}

/** Not confirmed: nothing is lost, and the page comes back by itself. */
async function restart(context: SectionContext): Promise<void> {
  await context.leaveAfter(
    "maintenance",
    () => context.client.restartHub(),
    "The bridge is restarting. This page reconnects when it's back, in about 10 seconds.",
    true,
  );
}

async function forgetWifi(context: SectionContext): Promise<void> {
  const ssid = context.state.stats?.wifi.ssid;
  const sure = await context.confirm({
    title: "Forget Wi-Fi?",
    body: [
      `The bridge forgets ${ssid ? `"${ssid}"` : "its Wi-Fi network"} and its password, then restarts into its setup network.`,
      "Everything else is kept: its name, the DSPi connection, the PIN and paired browsers and apps.",
      `To use it again, join the Wi-Fi network whose name starts with "DSPi Bridge" from a phone, and set it up.`,
    ],
    confirmLabel: "Forget Wi-Fi",
    danger: true,
  });
  if (!sure) return;
  await context.leaveAfter(
    "maintenance",
    () => context.client.resetHub("network"),
    `The bridge has left your network. ${SETUP_AGAIN}`,
    false,
  );
}

async function factoryReset(context: SectionContext): Promise<void> {
  const sure = await context.confirm({
    title: "Factory reset the bridge?",
    body: [
      "The bridge erases everything: its Wi-Fi network and password, its name and hostname, the DSPi connection, " +
        "the PIN and every paired browser and app.",
      "It then restarts as new, into its setup network. Your DSPi's own settings are not touched.",
    ],
    confirmLabel: "Erase everything",
    danger: true,
  });
  if (!sure) return;
  await context.leaveAfter(
    "maintenance",
    () => context.client.resetHub("factory"),
    `The bridge has been reset. ${SETUP_AGAIN}`,
    false,
  );
}
