/*
 * The settings page's Security section (bridge spec 10.3): the PIN switch,
 * changing the PIN, and the browsers and apps paired with it. Uses
 * hub.auth.set (Link spec 7.12) and auth.list and auth.revoke (Link spec
 * 7.3). Turning the PIN off or changing it signs out other clients, so both
 * are confirmed (bridge spec 10.4).
 */

import type { AuthClientInfo } from "@dspi/link";
import { el } from "../dom.js";
import { PIN_LENGTH, PIN_REMINDER, PIN_SWITCH_HELP, PIN_SWITCH_LABEL, pinIsValid } from "../pin.js";
import { pinInput, timeAgo } from "./fields.js";
import type { SectionContext } from "./state.js";

/**
 * Draws the Security section from the state.
 *
 * @param context The page's context.
 */
export function drawSecurity(context: SectionContext): void {
  const { state } = context;
  context.add("security", ...pinSwitch(context));

  if (state.pinStep === "turning_on") {
    context.add(
      "security",
      pinForm(
        context,
        "Turn on PIN",
        "Browsers and apps connected now, other than this one, are disconnected until they enter the PIN.",
      ),
    );
    return;
  }
  if (!state.pinOn) return;

  if (state.pinStep === "changing") {
    context.add(
      "security",
      pinForm(
        context,
        "Save new PIN",
        "Every other browser and app will need to enter the new PIN.",
      ),
    );
  } else {
    context.add(
      "security",
      el("button", {
        type: "button",
        text: "Change PIN",
        onClick: () => startPinForm(context, "changing"),
      }),
    );
  }
  context.add("security", el("h3", { text: "Paired browsers and apps" }), clientList(context));
}

/** The switch and the words under it. */
function pinSwitch(context: SectionContext): HTMLElement[] {
  const { state } = context;
  const toggle = el("input", {
    type: "checkbox",
    id: "pin-switch",
    role: "switch",
    checked: state.pinOn || state.pinStep === "turning_on",
    disabled: state.busy.has("security"),
    "aria-describedby": "pin-switch-help",
    onChange: (event: Event) => {
      const on = (event.target as HTMLInputElement).checked;
      if (state.pinOn && !on) void turnPinOff(context);
      else if (on) startPinForm(context, "turning_on");
      else cancelPinForm(context);
    },
  });
  return [
    el(
      "label",
      { class: "switch", for: "pin-switch" },
      el("span", { text: PIN_SWITCH_LABEL }),
      toggle,
    ),
    el("p", { class: "dim small", id: "pin-switch-help", text: PIN_SWITCH_HELP }),
  ];
}

function startPinForm(context: SectionContext, step: "turning_on" | "changing"): void {
  const { state } = context;
  state.pinStep = step;
  state.newPin = "";
  state.newPinConfirm = "";
  context.draw("security");
  document.getElementById("new-pin")?.focus();
}

function cancelPinForm(context: SectionContext): void {
  context.state.pinStep = "idle";
  context.draw("security");
  document.getElementById("pin-switch")?.focus();
}

/** The new PIN, its confirmation, and the button that applies it. */
function pinForm(context: SectionContext, submitLabel: string, consequence: string): HTMLElement {
  const { state } = context;
  const submit = el("button", { type: "button", class: "primary", text: submitLabel });
  const hint = el("p", { class: "small", role: "status" });
  const refresh = (): void => {
    const mismatch =
      state.newPinConfirm.length === PIN_LENGTH && state.newPinConfirm !== state.newPin;
    hint.textContent = mismatch ? "The two PINs don't match." : "";
    submit.disabled =
      state.busy.has("security") ||
      !pinIsValid(state.newPin) ||
      state.newPinConfirm !== state.newPin;
  };
  submit.addEventListener("click", () => void setPin(context, state.newPin));
  const form = el(
    "div",
    { class: "pin-form" },
    pinInput("new-pin", "PIN (6 digits)", state.newPin, (value) => {
      state.newPin = value;
      refresh();
    }),
    pinInput("new-pin-confirm", "Enter the PIN again", state.newPinConfirm, (value) => {
      state.newPinConfirm = value;
      refresh();
    }),
    hint,
    el("p", { class: "small", text: PIN_REMINDER }),
    el("p", { class: "dim small", text: consequence }),
    el(
      "div",
      { class: "row" },
      submit,
      el("button", { type: "button", text: "Cancel", onClick: () => cancelPinForm(context) }),
    ),
  );
  refresh();
  return form;
}

async function setPin(context: SectionContext, pin: string): Promise<void> {
  const { client, state, options } = context;
  // Changing a PIN that is already on signs out every other paired client.
  if (state.pinOn) {
    const sure = await context.confirm({
      title: "Change the PIN?",
      body: [
        "Every other paired browser and app is signed out, and needs the new PIN to connect again.",
        "This browser stays connected.",
      ],
      confirmLabel: "Change PIN",
      danger: true,
    });
    if (!sure) return;
  }
  const worked = await context.run("security", async () => {
    options.onToken(await client.setAuth("pin", pin));
    state.pinOn = true;
    state.pinStep = "idle";
    state.newPin = "";
    state.newPinConfirm = "";
    state.clients = await client.listClients();
  });
  if (worked) context.draw("security");
}

async function turnPinOff(context: SectionContext): Promise<void> {
  const { client, state, options } = context;
  const sure = await context.confirm({
    title: "Turn off the PIN?",
    body: [
      "Anyone on your network will be able to control your DSPi and change these settings.",
      "Every paired browser and app is forgotten. If you turn the PIN on again, each one enters it again.",
    ],
    confirmLabel: "Turn off PIN",
    danger: true,
  });
  if (!sure) {
    context.draw("security"); // Put the switch back on.
    return;
  }
  await context.run("security", async () => {
    await client.setAuth("none");
    options.onToken(null);
    state.pinOn = false;
    state.pinStep = "idle";
    state.clients = [];
  });
}

/** The paired clients, each with a Remove button. */
function clientList(context: SectionContext): HTMLElement {
  const { state } = context;
  if (state.clients.length === 0) {
    return el("p", { class: "dim", text: "None yet." });
  }
  const list = el("ul", { class: "client-list" });
  for (const paired of state.clients) {
    list.appendChild(
      el(
        "li",
        {},
        el(
          "span",
          { class: "grow" },
          el("strong", { text: paired.name }),
          el("br"),
          el("span", { class: "dim small", text: lastSeen(paired) }),
        ),
        el("button", {
          type: "button",
          class: "danger",
          text: "Remove",
          "aria-label": `Remove ${paired.name}`,
          disabled: state.busy.has("security"),
          onClick: () => void removeClient(context, paired),
        }),
      ),
    );
  }
  return list;
}

function lastSeen(paired: AuthClientInfo): string {
  if (paired.online) return "Connected now";
  if (paired.last_seen) return `Last seen ${timeAgo(paired.last_seen)}`;
  return "Not seen since pairing";
}

async function removeClient(context: SectionContext, paired: AuthClientInfo): Promise<void> {
  const { client, state } = context;
  const sure = await context.confirm({
    title: `Remove ${paired.name}?`,
    body: [
      `${paired.name} will need the PIN to connect again. If it is connected now, it is disconnected.`,
      "If this is the browser you are using, this page disconnects too.",
    ],
    confirmLabel: "Remove",
    danger: true,
  });
  if (!sure) return;
  await context.run("security", async () => {
    await client.revokeClient(paired.cid);
    state.clients = await client.listClients();
  });
}
