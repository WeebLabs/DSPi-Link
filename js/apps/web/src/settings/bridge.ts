/*
 * The settings page's Bridge section (bridge spec 10.3): the bridge's name,
 * hostname and status LED, saved through hub.config.set (Link spec 7.10; keys
 * in bridge spec 8.11 and the LED in bridge spec 13).
 */

import { LINK_DEFAULT_PORT } from "@dspi/link";
import { el } from "../dom.js";
import { statusLine } from "../status.js";
import { pinChoices, sameJson, selectField, textField } from "./fields.js";
import {
  readBridgeConfig,
  type BridgeConfig,
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

type BridgeValues = Omit<BridgeConfig, "uart">;

/**
 * Draws the Bridge section from the state.
 *
 * @param context The page's context.
 */
export function drawBridge(context: SectionContext): void {
  const { state } = context;
  if (!state.config || !state.choices) return;
  const saved: BridgeValues = {
    name: state.config.name,
    hostname: state.config.hostname,
    led: state.config.led,
  };
  const draft = (): BridgeValues => state.bridgeDraft ?? saved;
  const edit = (change: Partial<BridgeValues>): void => {
    state.bridgeDraft = { ...draft(), ...change };
    refresh();
  };

  const hostnameHelp = el("p", { class: "dim small" });
  const ledPin = el("div");
  const hint = el("div", { role: "status" });
  const save = el("button", { type: "button", class: "primary", text: "Save" });
  const undo = el("button", { type: "button", text: "Undo" });
  const refresh = (): void => {
    const values = draft();
    hostnameHelp.textContent =
      "This bridge's address on your network: " +
      `http://${values.hostname || "…"}.local:${LINK_DEFAULT_PORT}`;
    ledPin.hidden = values.led.type === "none";
    const problem = bridgeProblem(values, context);
    hint.replaceChildren(...(problem ? [statusLine("bad", problem)] : []));
    const changed = state.bridgeDraft !== null && !sameJson(state.bridgeDraft, saved);
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

async function saveBridge(context: SectionContext): Promise<void> {
  const { client, state } = context;
  const change = state.bridgeDraft;
  if (!change) return;
  await context.run("bridge", async () => {
    const reply = await client.setHubConfig(change);
    const config = readBridgeConfig(reply);
    if (!config) throw new Error("the bridge's reply could not be read");
    state.config = config;
    state.bridgeDraft = null;
  });
}
