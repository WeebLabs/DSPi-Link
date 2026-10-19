/*
 * The bridge's setup page (bridge spec 10.2): joins the bridge to a home
 * network from a phone on its setup network, through the setup API (bridge
 * spec 9.3, api.ts).
 *
 *   welcome     -> what the bridge found, and a Get started button
 *   network     -> nearby networks, or a name typed by hand
 *   password    -> the chosen network's password
 *   name        -> the bridge's name, and whether it asks for a PIN
 *   connecting  -> polls the bridge until the join works or fails
 *   done        -> the bridge's new address, and Finish
 *
 * One `flow` object holds everything the page knows, and render() draws the
 * current step from it. Every action changes `flow` and calls render().
 */

import "./setup.css";
import { LINK_DEFAULT_PORT } from "@dspi/link";
import { clear, el } from "../dom.js";
import {
  digitsOnly,
  PIN_LENGTH,
  PIN_REMINDER,
  PIN_SWITCH_HELP,
  PIN_SWITCH_LABEL,
  pinIsValid,
} from "../pin.js";
import { statusLine } from "../status.js";
import {
  namedNetworks,
  passwordFits,
  signalBars,
  SSID_MAX_BYTES,
  WIFI_PASSWORD_MAX_LENGTH,
} from "../wifi.js";
import {
  apply,
  finish,
  getNetworks,
  getState,
  SetupError,
  type Network,
  type PinChange,
  type SetupState,
} from "./api.js";

type Step = "welcome" | "network" | "password" | "name" | "connecting" | "done";

/** Which segment of the progress bar each step lights. */
const STEP_NUMBER: Record<Step, number> = {
  welcome: 1,
  network: 2,
  password: 2,
  name: 3,
  connecting: 4,
  done: 5,
};
const STEP_COUNT = 5;

/** How often the connecting step asks the bridge how the join is going. */
const POLL_INTERVAL_MS = 1000;

/** The bridge's setup network drops for a moment while it changes channel to
 * join the home network, so a few failed polls in a row are expected. */
const POLL_FAILURES_ALLOWED = 8;

/** The bridge tries a network for up to 30 s (Link spec 7.11); this adds room
 * for the setup network's own drop-outs before the page gives up. */
const JOIN_DEADLINE_MS = 45000;

/** The longest name the bridge stores (bridge spec 8.11). */
const BRIDGE_NAME_MAX_LENGTH = 63;

/** The bars a signal can show. */
const SIGNAL_BAR_COUNT = 4;
const SIGNAL_BAR_STEP_PX = 4;

const flow = {
  step: "welcome" as Step,
  state: null as SetupState | null,
  networks: null as Network[] | null, // null until the first scan finishes
  scanning: false,
  typingName: false, // The user chose to type a network name
  ssid: "",
  secure: true,
  password: "",
  showPassword: false,
  name: "",
  // The PIN (bridge spec 10.2). `currentPin` is asked for only when the
  // bridge already has one, as after "Forget Wi-Fi".
  pinEnabled: false,
  pin: "",
  pinConfirm: "",
  currentPin: "",
  pinAfterApply: "", // The bridge's PIN once apply has worked, for finish
  error: null as string | null,
  /** True while /apply is in flight, so a second tap sends nothing. */
  applying: false,
  finished: false,
};

const root = document.getElementById("setup")!;

// ---------------------------------------------------------------------------
// Pieces shared by every step

function brand(): HTMLElement {
  return el(
    "div",
    { class: "brand" },
    el("span", { class: "brand-mark", "aria-hidden": "true", text: "D" }),
    "DSPi Bridge setup",
  );
}

function progress(): HTMLElement {
  const current = STEP_NUMBER[flow.step];
  const bar = el("div", {
    class: "steps",
    role: "progressbar",
    "aria-valuemin": "1",
    "aria-valuemax": String(STEP_COUNT),
    "aria-valuenow": String(current),
    "aria-label": `Step ${current} of ${STEP_COUNT}`,
  });
  for (let step = 1; step <= STEP_COUNT; step++) {
    bar.appendChild(el("span", { class: step <= current ? "done" : "" }));
  }
  return bar;
}

/** The nodes given, without the nulls that stand for "nothing here". */
function present(...nodes: (HTMLElement | null)[]): HTMLElement[] {
  return nodes.filter((node): node is HTMLElement => node !== null);
}

function errorStatus(): HTMLElement | null {
  // The page's <main> is a polite live region, so only errors need role "alert".
  return flow.error ? statusLine("bad", flow.error, undefined, true) : null;
}

interface ButtonOptions {
  primary?: boolean;
  disabled?: boolean;
  small?: boolean;
}

function button(
  label: string,
  onClick: () => void,
  options: ButtonOptions = {},
): HTMLButtonElement {
  const classes = ["button", options.primary ? "primary" : "", options.small ? "small" : ""];
  return el("button", {
    type: "button",
    class: classes.filter(Boolean).join(" "),
    text: label,
    disabled: options.disabled ?? false,
    onclick: onClick,
  }) as HTMLButtonElement;
}

/** A secondary action drawn as a link. */
function linkButton(label: string, onClick: () => void): HTMLButtonElement {
  return el("button", {
    type: "button",
    class: "link-button",
    text: label,
    onclick: onClick,
  }) as HTMLButtonElement;
}

// ---------------------------------------------------------------------------
// Steps

function welcome(): HTMLElement[] {
  const dspi = flow.state?.dspi;
  const found = dspi?.found
    ? statusLine("good", "DSPi found", dspi.serial ? `Serial ${dspi.serial}` : undefined)
    : statusLine(
        "warn",
        "DSPi not found yet",
        "Check the three wires, and that the UART control interface is on in DSPi Console. " +
          "You can carry on and fix this later.",
      );
  const notifications =
    dspi?.found && dspi.notify_enable === false
      ? statusLine(
          "warn",
          "Notifications are off",
          "Turn them on in DSPi Console's UART settings so changes show up straight away.",
        )
      : null;

  return present(
    el("h1", { text: "Let's connect your DSPi Bridge to your Wi-Fi." }),
    el("p", { class: "dim", text: "It takes about a minute." }),
    errorStatus(),
    found,
    notifications,
    button("Get started", () => goToNetworks(), { primary: true }),
  );
}

/** The signal as bars; the button's label says it in words. */
function signalIcon(rssi: number): HTMLElement {
  const strength = signalBars(rssi);
  const bars = el("span", { class: "bars", "aria-hidden": "true" });
  for (let bar = 1; bar <= SIGNAL_BAR_COUNT; bar++) {
    bars.appendChild(
      el("i", {
        class: bar <= strength ? "on" : "",
        style: `height: ${bar * SIGNAL_BAR_STEP_PX}px`,
      }),
    );
  }
  return bars;
}

/** A padlock for a secured network. Fixed markup, nothing from the network. */
function lockIcon(): HTMLElement {
  const icon = el("span", { class: "lock", "aria-hidden": "true" });
  icon.innerHTML =
    '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
    'stroke-width="2.4" stroke-linecap="round"><rect x="5" y="11" width="14" height="10" rx="2"/>' +
    '<path d="M8 11V8a4 4 0 0 1 8 0v3"/></svg>';
  return icon;
}

function networkList(): HTMLElement {
  if (flow.networks === null || flow.scanning) {
    return el(
      "div",
      { class: "center" },
      el("div", { class: "spinner", "aria-hidden": "true" }),
      el("p", { class: "dim", text: "Looking for networks…" }),
    );
  }
  if (flow.networks.length === 0) {
    return el("p", { class: "dim", text: "No networks found. Refresh, or enter a network name." });
  }
  const list = el("ul", { class: "networks" });
  for (const network of flow.networks) {
    const label =
      `${network.ssid}, ${network.secure ? "secured" : "open"}, ` +
      `signal ${signalBars(network.rssi)} of ${SIGNAL_BAR_COUNT}`;
    list.appendChild(
      el(
        "li",
        {},
        el(
          "button",
          {
            type: "button",
            "aria-label": label,
            onclick: () => chooseNetwork(network.ssid, network.secure),
          },
          signalIcon(network.rssi),
          el("span", { class: "ssid", text: network.ssid }),
          network.secure ? lockIcon() : null,
        ),
      ),
    );
  }
  return list;
}

function networkStep(): HTMLElement[] {
  if (flow.typingName) {
    const field = el("input", {
      type: "text",
      id: "ssid",
      value: flow.ssid,
      autocomplete: "off",
      autocapitalize: "off",
      spellcheck: false,
      maxLength: SSID_MAX_BYTES,
      oninput: (event: Event) => {
        flow.ssid = (event.target as HTMLInputElement).value;
        continueButton.disabled = flow.ssid.trim() === "";
      },
    });
    const continueButton = button("Continue", () => chooseNetwork(flow.ssid.trim(), true), {
      primary: true,
      disabled: flow.ssid.trim() === "",
    });
    return present(
      el("h1", { text: "Enter your network's name" }),
      el("p", {
        class: "dim",
        text: "Use this for a network that hides its name. Only 2.4 GHz networks work.",
      }),
      errorStatus(),
      el("div", { class: "field" }, el("label", { for: "ssid", text: "Network name" }), field),
      continueButton,
      button("Back to the list", () => {
        flow.typingName = false;
        render();
      }),
    );
  }

  return present(
    el("h1", { text: "Choose your network" }),
    el("p", { class: "dim", text: "Only 2.4 GHz networks are shown." }),
    errorStatus(),
    networkList(),
    el(
      "div",
      { class: "input-row" },
      button("Refresh", () => void scan(), { small: true, disabled: flow.scanning }),
      linkButton("Enter a network name", () => {
        flow.typingName = true;
        flow.error = null;
        render();
      }),
    ),
  );
}

function passwordStep(): HTMLElement[] {
  const field = el("input", {
    type: flow.showPassword ? "text" : "password",
    id: "password",
    value: flow.password,
    autocomplete: "current-password",
    autocapitalize: "off",
    spellcheck: false,
    maxLength: WIFI_PASSWORD_MAX_LENGTH,
    oninput: (event: Event) => {
      flow.password = (event.target as HTMLInputElement).value;
      continueButton.disabled = !passwordFits(flow.password, flow.secure);
    },
  }) as HTMLInputElement;
  const toggle = button(
    flow.showPassword ? "Hide" : "Show",
    () => {
      flow.showPassword = !flow.showPassword;
      render();
      document.getElementById("password")?.focus();
    },
    { small: true },
  );
  toggle.setAttribute("aria-controls", "password");
  const continueButton = button("Continue", () => goTo("name"), {
    primary: true,
    disabled: !passwordFits(flow.password, flow.secure),
  });

  return present(
    el("h1", { text: `Password for "${flow.ssid}"` }),
    errorStatus(),
    el(
      "div",
      { class: "field" },
      el("label", { for: "password", text: "Wi-Fi password" }),
      el("div", { class: "input-row" }, field, toggle),
    ),
    flow.secure
      ? null
      : el("p", { class: "dim small", text: "This network is open; leave the password empty." }),
    continueButton,
    button("Choose another network", () => goToNetworks()),
  );
}

/** A 6-digit PIN field. `onInput` runs after each change to `value`. */
function pinField(
  id: string,
  label: string,
  value: string,
  onInput: (value: string) => void,
): HTMLElement {
  const input = el("input", {
    type: "password",
    id,
    value,
    inputMode: "numeric",
    pattern: "[0-9]*",
    autocomplete: "off",
    maxLength: PIN_LENGTH,
    oninput: (event: Event) => {
      const field = event.target as HTMLInputElement;
      field.value = digitsOnly(field.value);
      onInput(field.value);
    },
  });
  return el(
    "div",
    { class: "field" },
    el("label", { for: id, text: label }),
    el("div", { class: "pin-input" }, input),
  );
}

/** The switch that turns the PIN on and off, with the wording that says
 * what it protects. */
function pinSwitch(): HTMLElement {
  const input = el("input", {
    type: "checkbox",
    id: "pin-switch",
    role: "switch",
    checked: flow.pinEnabled,
    "aria-describedby": "pin-switch-help",
    onchange: (event: Event) => {
      flow.pinEnabled = (event.target as HTMLInputElement).checked;
      render();
      document.getElementById("pin-switch")?.focus();
    },
  });
  return el(
    "div",
    { class: "switch-row" },
    el(
      "label",
      { class: "switch", for: "pin-switch" },
      el("span", { text: PIN_SWITCH_LABEL }),
      input,
    ),
    el("p", { class: "dim small", id: "pin-switch-help", text: PIN_SWITCH_HELP }),
  );
}

/** Whether the bridge has a PIN now, before this setup changes anything. */
function bridgeHasPin(): boolean {
  return flow.state?.pin_required ?? false;
}

/** With a PIN already set and the switch left on, an empty new PIN keeps it. */
function keepingCurrentPin(): boolean {
  return bridgeHasPin() && flow.pinEnabled && flow.pin === "" && flow.pinConfirm === "";
}

/** Why the name step cannot go on yet, or null when it can. */
function nameStepProblem(): string | null {
  if (flow.name.trim() === "") {
    return "Give the bridge a name.";
  }
  if (bridgeHasPin() && !pinIsValid(flow.currentPin)) {
    return "Enter the bridge's current PIN.";
  }
  if (!flow.pinEnabled || keepingCurrentPin()) {
    return null;
  }
  if (!pinIsValid(flow.pin)) {
    return "Choose a PIN of 6 digits.";
  }
  if (flow.pinConfirm !== flow.pin) {
    return flow.pinConfirm.length === PIN_LENGTH
      ? "The two PINs don't match."
      : "Enter the PIN again.";
  }
  return null;
}

/** The bridge's current PIN, asked for when it already has one. */
function currentPinFields(update: () => void): HTMLElement {
  return el(
    "div",
    {},
    el("p", {
      class: "dim small",
      text: "This bridge already has a PIN. Enter it to change the bridge's settings.",
    }),
    pinField("current-pin", "Current PIN", flow.currentPin, (value) => {
      flow.currentPin = value;
      update();
    }),
  );
}

/** The new PIN and its confirmation, shown while the switch is on. */
function newPinFields(update: () => void): HTMLElement {
  return el(
    "div",
    { class: "pin-fields" },
    pinField("pin", bridgeHasPin() ? "New PIN" : "PIN", flow.pin, (value) => {
      flow.pin = value;
      update();
    }),
    pinField("pin-confirm", "Enter the PIN again", flow.pinConfirm, (value) => {
      flow.pinConfirm = value;
      update();
    }),
    bridgeHasPin()
      ? el("p", { class: "dim small", text: "Leave these empty to keep the current PIN." })
      : null,
    el("p", { class: "small", text: PIN_REMINDER }),
  );
}

function nameStep(): HTMLElement[] {
  // The fields update the Connect button and the hint as they change, without
  // redrawing the step, so typing never loses focus.
  const hint = el("p", { class: "dim small", role: "status" });
  const update = (): void => {
    const problem = nameStepProblem();
    connectButton.disabled = problem !== null || flow.applying;
    // Only the mismatch is worth saying out loud; the rest is plain from
    // the empty fields.
    hint.textContent = problem === "The two PINs don't match." ? problem : "";
  };
  const nameInput = el("input", {
    type: "text",
    id: "name",
    value: flow.name,
    maxLength: BRIDGE_NAME_MAX_LENGTH,
    oninput: (event: Event) => {
      flow.name = (event.target as HTMLInputElement).value;
      update();
    },
  });
  const connectButton = button(flow.applying ? "Connecting…" : "Connect", () => void connect(), {
    primary: true,
  });

  const currentPin = bridgeHasPin() ? currentPinFields(update) : null;
  const newPin = flow.pinEnabled ? newPinFields(update) : null;

  update();
  return present(
    el("h1", { text: "Name and security" }),
    el("div", { class: "field" }, el("label", { for: "name", text: "Bridge name" }), nameInput),
    el("p", {
      class: "dim small",
      text: "DSPi Console and other apps show this name. You can change it later.",
    }),
    currentPin,
    pinSwitch(),
    newPin,
    hint,
    // This step is long on a phone, so a refusal (a wrong PIN, say) shows by
    // the button that caused it rather than out of sight at the top.
    errorStatus(),
    connectButton,
    button("Back", () => goTo(flow.secure ? "password" : "network")),
  );
}

function connectingStep(): HTMLElement[] {
  return present(
    el("div", { class: "spinner", "aria-hidden": "true" }),
    el("h1", { class: "center", text: `Joining "${flow.ssid}"…` }),
    el("p", { class: "dim center", text: "This takes up to half a minute. Keep this page open." }),
  );
}

function doneStep(): HTMLElement[] {
  const wifi = flow.state?.wifi;
  const hostname = wifi?.hostname ?? "dspi-bridge";
  const address = `http://${hostname}.local:${LINK_DEFAULT_PORT}`;
  if (flow.finished) {
    return present(
      statusLine("good", "All set"),
      el("p", {
        text:
          `The setup network is turning off. Reconnect this device to "${flow.ssid}", ` +
          "then open:",
      }),
      el("a", { class: "address", href: address, text: address }),
    );
  }
  return present(
    el("h1", { text: `Your bridge is on "${flow.ssid}".` }),
    el("p", { class: "dim", text: "Open it from any device on that network at:" }),
    el("a", { class: "address", href: address, text: address }),
    wifi?.ip
      ? el("p", {
          class: "dim small",
          text: `If that doesn't open, use http://${wifi.ip}:${LINK_DEFAULT_PORT}`,
        })
      : null,
    el("p", {
      text:
        `Reconnect this device to "${flow.ssid}" to open it. ` +
        "DSPi Console will find the bridge by itself.",
    }),
    button("Finish", () => void done(), { primary: true }),
  );
}

// ---------------------------------------------------------------------------
// Drawing

const STEPS: Record<Step, () => HTMLElement[]> = {
  welcome,
  network: networkStep,
  password: passwordStep,
  name: nameStep,
  connecting: connectingStep,
  done: doneStep,
};

function render(): void {
  clear(root);
  const card = el("section", { class: "card" });
  for (const node of STEPS[flow.step]()) {
    card.appendChild(node);
  }
  const version = flow.state ? `Bridge firmware ${flow.state.version} on ${flow.state.chip}` : "";
  root.append(brand(), progress(), card, el("p", { class: "footer", text: version }));
}

/** Moves focus to the new step's heading, so screen readers announce it. */
function focusHeading(): void {
  const heading = root.querySelector("h1");
  if (heading) {
    heading.setAttribute("tabindex", "-1");
    heading.focus({ preventScroll: true });
  }
}

/** Shows `step`, with `error` above it if something went wrong. */
function goTo(step: Step, error: string | null = null): void {
  flow.step = step;
  flow.error = error;
  render();
  focusHeading();
}

// ---------------------------------------------------------------------------
// Actions

function goToNetworks(): void {
  goTo("network");
  if (flow.networks === null) {
    void scan();
  }
}

async function scan(): Promise<void> {
  flow.scanning = true;
  render();
  try {
    flow.networks = namedNetworks(await getNetworks());
  } catch (error) {
    flow.networks = flow.networks ?? [];
    flow.error = (error as Error).message;
  } finally {
    flow.scanning = false;
    render();
  }
}

function chooseNetwork(ssid: string, secure: boolean): void {
  flow.ssid = ssid;
  flow.secure = secure;
  flow.typingName = false;
  flow.password = "";
  goTo(secure ? "password" : "name");
}

/** What the name step asks `apply` to do to the PIN. */
function pinChange(): PinChange {
  const change: PinChange = {};
  if (bridgeHasPin()) {
    change.current_pin = flow.currentPin;
  }
  if (!flow.pinEnabled) {
    change.pin_enabled = false;
  } else if (!keepingCurrentPin()) {
    change.pin_enabled = true;
    change.pin = flow.pin;
  }
  return change;
}

/** The PIN the bridge will have once `apply` has worked. */
function pinOnceApplied(): string {
  if (!flow.pinEnabled) {
    return "";
  }
  return keepingCurrentPin() ? flow.currentPin : flow.pin;
}

async function connect(): Promise<void> {
  if (flow.applying) return;
  flow.applying = true;
  flow.error = null;
  render();
  try {
    await apply(flow.ssid, flow.password, flow.name.trim(), pinChange());
    flow.pinAfterApply = pinOnceApplied();
  } catch (error) {
    flow.error = (error as Error).message;
    return;
  } finally {
    flow.applying = false;
    render();
  }
  goTo("connecting");
  void waitForJoin();
}

/** What to tell the user when a join fails, and which step to send them to. */
function explainFailure(failure: SetupState["wifi"]["failure"]): { message: string; step: Step } {
  switch (failure) {
    case "wrong_password":
      return { message: "That password didn't work. Check it and try again.", step: "password" };
    case "not_found":
      return {
        message: `Couldn't find "${flow.ssid}". Make sure it's a 2.4 GHz network and in range.`,
        step: "network",
      };
    default:
      return {
        message: `Couldn't join "${flow.ssid}". Check the password and try again.`,
        step: flow.secure ? "password" : "network",
      };
  }
}

/** Polls the bridge until the join works, fails, or the deadline passes. */
async function waitForJoin(): Promise<void> {
  const deadline = Date.now() + JOIN_DEADLINE_MS;
  let failures = 0;
  for (;;) {
    if (Date.now() > deadline) {
      goTo("network", "The bridge took too long to join. Check the network and try again.");
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, POLL_INTERVAL_MS));
    let state: SetupState;
    try {
      state = await getState();
      failures = 0;
    } catch {
      if (++failures > POLL_FAILURES_ALLOWED) {
        goTo("network", "The bridge stopped answering. Rejoin its setup network and try again.");
        return;
      }
      continue;
    }
    flow.state = state;
    if (state.wifi.join === "connected") {
      goTo("done");
      return;
    }
    if (state.wifi.join === "failed") {
      const { message, step } = explainFailure(state.wifi.failure);
      goTo(step, message);
      return;
    }
  }
}

async function done(): Promise<void> {
  try {
    await finish(flow.pinAfterApply);
  } catch (error) {
    // Not worth troubling the user: the setup network goes off by itself two
    // minutes after the join anyway.
    console.warn("setup: finish failed:", error);
  }
  flow.finished = true;
  render();
}

// ---------------------------------------------------------------------------
// Start

async function start(): Promise<void> {
  render();
  try {
    flow.state = await getState();
  } catch (error) {
    flow.error = error instanceof SetupError ? error.message : "The bridge isn't answering.";
  }
  flow.name = flow.state?.name ?? "DSPi Bridge";
  flow.pinEnabled = flow.state?.pin_required ?? false;
  if (flow.state?.wifi.join === "connected" && flow.state.wifi.saved_ssid) {
    // The page was reopened after a join that worked.
    flow.ssid = flow.state.wifi.saved_ssid;
    flow.step = "done";
  }
  render();
}

void start();
