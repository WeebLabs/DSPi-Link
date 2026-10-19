/*
 * The settings page's Bridge section (bridge spec 10.3): the bridge's name,
 * hostname and status LED, saved through hub.config.set (Link spec 7.10; keys
 * in bridge spec 8.11 and the LED in bridge spec 13).
 */

import { LINK_DEFAULT_PORT, type HubConfig } from "@dspi/link";
import { el } from "../dom.js";
import { statusLine } from "../status.js";
import { pinChoices, selectField, textField } from "./fields.js";
import {
  readBridgeConfig,
  type BridgeValues,
  type LedConfig,
  type LedType,
  type SectionContext,
} from "./state.js";

/** The longest name and hostname the bridge stores (bridge spec 8.11). */
const NAME_MAX_BYTES = 63;
const HOSTNAME_MAX_LENGTH = 63;

/** A DNS label: lower-case letters, digits and inner hyphens, at most 63 characters. */
const HOSTNAME_PATTERN = /^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$/;

const LED_TYPE_LABELS: Record<LedType, string> = {
  none: "No LED",
  gpio: "LED on a pin, lit when the pin is high",
  gpio_active_low: "LED on a pin, lit when the pin is low",
  ws2812: "Addressable RGB LED (WS2812)",
};

/**
 * Draws the Bridge section from the state.
 *
 * @param context The page's context.
 */
export function drawBridge(context: SectionContext): void {
  const { state } = context;
  if (!state.config || !state.choices) return;
  const shown = state.config;
  // The saved values are read afresh each time: another admin may change
  // them while this user edits (hub.config.changed).
  const draft = (): BridgeValues => {
    const { name, hostname, led } = state.config ?? shown;
    return { name, hostname, led, ...state.bridgeDraft };
  };
  const edit = (change: Partial<BridgeValues>): void => {
    state.bridgeDraft = { ...state.bridgeDraft, ...change };
    refresh();
  };

  if (state.movedTo) {
    context.add(
      "bridge",
      el(
        "p",
        { role: "status" },
        "The bridge's address is now ",
        el("a", { href: state.movedTo, text: state.movedTo }),
        ". Open it there to carry on; the old address no longer reaches the bridge.",
      ),
    );
  }

  const hostnameHelp = el("p", { class: "dim small" });
  const ledPin = el("div");
  const hint = el("div", { role: "status" });
  const save = el("button", { type: "button", class: "primary", text: "Save" });
  const undo = el("button", { type: "button", text: "Undo" });
  const refresh = (): void => {
    const values = draft();
    hostnameHelp.textContent =
      "This bridge's address on your network: " + addressOf(values.hostname || "…");
    ledPin.hidden = values.led.type === "none";
    const problem = bridgeProblem(values, context);
    hint.replaceChildren(...(problem ? [statusLine("bad", problem)] : []));
    const changed = bridgeChanges(context) !== null;
    save.disabled = !changed || problem !== null || state.busy.has("bridge");
    undo.hidden = !changed;
  };

  save.addEventListener("click", () => void saveBridge(context));
  undo.addEventListener("click", () => {
    state.bridgeDraft = null;
    context.draw("bridge");
  });

  const hostnameField = textField(
    "bridge-hostname",
    "Hostname",
    draft().hostname,
    HOSTNAME_MAX_LENGTH,
    (value) => edit({ hostname: value.toLowerCase() }),
  );
  hostnameField.appendChild(hostnameHelp);
  ledPin.appendChild(ledPinField(context, draft, edit));

  context.add(
    "bridge",
    textField(
      "bridge-name",
      "Name",
      draft().name,
      NAME_MAX_BYTES,
      (value) => edit({ name: value }),
      "DSPi Console and other apps show this name.",
    ),
    hostnameField,
    ledTypeField(context, draft, edit),
    ledPin,
    hint,
    el("div", { class: "row" }, save, undo),
  );
  refresh();
}

/** The status LED's kind. Choosing one with a pin keeps the pin, or offers the first allowed. */
function ledTypeField(
  context: SectionContext,
  draft: () => BridgeValues,
  edit: (change: Partial<BridgeValues>) => void,
): HTMLElement {
  const choices = context.state.choices?.led ?? { type: [], pin: [] };
  return selectField(
    "led-type",
    "Status LED",
    choices.type.map((type) => ({ value: type, label: LED_TYPE_LABELS[type] ?? type })),
    draft().led.type,
    (value) => {
      const type = value as LedType;
      edit({ led: { type, pin: type === "none" ? null : (draft().led.pin ?? choices.pin[0]) } });
    },
    "A light on the bridge that shows how it is doing.",
  );
}

/** The status LED's pin, hidden by the caller when there is no LED. */
function ledPinField(
  context: SectionContext,
  draft: () => BridgeValues,
  edit: (change: Partial<BridgeValues>) => void,
): HTMLElement {
  const pins = context.state.choices?.led.pin ?? [];
  return selectField("led-pin", "LED pin", pinChoices(pins), draft().led.pin ?? pins[0], (value) =>
    edit({ led: { ...draft().led, pin: Number(value) } }),
  );
}

/** Why the Bridge section's values cannot be saved, or null when they can. */
function bridgeProblem(values: BridgeValues, context: SectionContext): string | null {
  const nameBytes = new TextEncoder().encode(values.name.trim()).length;
  if (nameBytes === 0) return "Give the bridge a name.";
  if (nameBytes > NAME_MAX_BYTES) return "The name is too long.";
  if (!HOSTNAME_PATTERN.test(values.hostname)) {
    return "A hostname uses only lower-case letters, digits and hyphens, and cannot start or end with a hyphen.";
  }
  if (ledUsesUartPin(values.led, context)) {
    return "The LED cannot use a pin wired to the DSPi.";
  }
  return null;
}

function ledUsesUartPin(led: LedConfig, context: SectionContext): boolean {
  const uart = context.state.config?.uart;
  if (led.pin === null || !uart) return false;
  return led.pin === uart.tx_pin || led.pin === uart.rx_pin;
}

/** The address the bridge's page and Link port answer at for `hostname`. */
function addressOf(hostname: string): string {
  return `http://${hostname}.local:${LINK_DEFAULT_PORT}`;
}

/**
 * The Bridge keys the user changed from their saved values, or null for
 * none. Only these are sent (Link spec 7.10), and of the LED only the parts
 * that changed, since nested objects merge.
 */
function bridgeChanges(context: SectionContext): HubConfig | null {
  const { bridgeDraft: draft, config } = context.state;
  if (!draft || !config) return null;
  const change: HubConfig = {};
  if (draft.name !== undefined && draft.name.trim() !== config.name) {
    change.name = draft.name.trim();
  }
  if (draft.hostname !== undefined && draft.hostname !== config.hostname) {
    change.hostname = draft.hostname;
  }
  if (draft.led) {
    const led: Partial<LedConfig> = {};
    if (draft.led.type !== config.led.type) led.type = draft.led.type;
    if (draft.led.pin !== config.led.pin) led.pin = draft.led.pin;
    if (Object.keys(led).length > 0) change.led = led;
  }
  return Object.keys(change).length > 0 ? change : null;
}

/** A new hostname moves the bridge's address, this page's included. */
function confirmHostname(
  context: SectionContext,
  previous: string,
  next: string,
): Promise<boolean> {
  return context.confirm({
    title: "Change the hostname?",
    body: [
      `The bridge's address becomes ${addressOf(next)}.`,
      `The old address, ${addressOf(previous)}, stops working at once, for bookmarks and for ` +
        "this page if you opened it there. DSPi Console finds the bridge by itself.",
    ],
    confirmLabel: "Change hostname",
  });
}

async function saveBridge(context: SectionContext): Promise<void> {
  const { client, state } = context;
  const change = bridgeChanges(context);
  if (!change || !state.config) return;
  const previousHostname = state.config.hostname;
  const nextHostname = typeof change.hostname === "string" ? change.hostname : null;
  if (nextHostname && !(await confirmHostname(context, previousHostname, nextHostname))) return;
  await context.run("bridge", async () => {
    const reply = await client.setHubConfig(change);
    const config = readBridgeConfig(reply);
    if (!config) throw new Error("the bridge's reply could not be read");
    state.config = config;
    state.bridgeDraft = null;
    if (nextHostname) state.movedTo = addressOf(config.hostname);
  });
}
