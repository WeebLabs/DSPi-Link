/*
 * The bridge's settings page (bridge spec 10.3), shown inside the control
 * page when the hub has the `hub_config` capability and this session is
 * `admin`. Everything goes through the Link admin messages (Link spec 7.9 to
 * 7.13); the page has no API of its own.
 *
 *   Status           what the bridge sees now, refreshed every few seconds
 *   DSPi connection  UART pins and speed, with a wiring diagram
 *   Wi-Fi            the current network, and moving to another one
 *   Security         the PIN, and the browsers and apps paired with it
 *   Bridge           name, hostname and status LED
 *   Maintenance      update firmware, restart, forget Wi-Fi, factory reset
 *
 * Each section draws into its own element from the `page` state below, so
 * redrawing one (new stats, a saved change) never disturbs a field being
 * typed in another. Inputs change the state and call their section's small
 * refresh(), which updates buttons and hints in place without redrawing.
 */

import {
  LinkClient,
  LinkProtocolError,
  type AuthClientInfo,
  type HubConfig,
  type WifiNetwork,
} from "@dspi/link";
import { confirmAction } from "./dialog.js";
import { clear, el } from "./dom.js";
import { chipLabel, readFirmwareInfo } from "./firmware.js";
import {
  digitsOnly,
  PIN_LENGTH,
  PIN_REMINDER,
  PIN_SWITCH_HELP,
  PIN_SWITCH_LABEL,
  pinIsValid,
} from "./pin.js";

// ---------------------------------------------------------------------------
// The bridge's configuration and stats (bridge spec 8.7 and 8.11)

interface UartConfig {
  tx_pin: number;
  rx_pin: number;
  baud: number;
  detect_baud: boolean;
}

type LedType = "none" | "gpio" | "gpio_active_low" | "ws2812";

interface LedConfig {
  type: LedType;
  pin: number | null;
}

interface BridgeConfig {
  name: string;
  hostname: string;
  uart: UartConfig;
  led: LedConfig;
}

interface BridgeOptions {
  uart: { tx_pin: number[]; rx_pin: number[]; baud: number[] };
  led: { type: LedType[]; pin: number[] };
}

/** The `bridge` object of hub.stats. */
interface BridgeStats {
  chip: string;
  uptime_s: number;
  wifi: { ssid: string; rssi: number; ip: string; mac: string };
  uart: {
    baud: number;
    live: boolean;
    notify_enable: boolean;
    timeouts: number;
    crc_errors: number;
    retries: number;
  };
}

/** How often the Status section asks for fresh numbers. */
const STATS_INTERVAL_MS = 5000;

/** Below this the bridge works, but slowly (bridge spec 5). */
const RECOMMENDED_MIN_BAUD = 921600;

/** The DSPi's own UART pins, as DSPi Console sets them by default (spec 5). */
const DSPI_TX_PIN = 16;
const DSPI_RX_PIN = 17;

const LED_TYPE_LABELS: Record<LedType, string> = {
  none: "No LED",
  gpio: "LED on a pin, lit when the pin is high",
  gpio_active_low: "LED on a pin, lit when the pin is low",
  ws2812: "Addressable RGB LED (WS2812)",
};

/** Names for the keys a rejected hub.config.set can name (Link spec 7.10). */
const FIELD_LABELS: Record<string, string> = {
  name: "name",
  hostname: "hostname",
  "uart.tx_pin": "TX pin",
  "uart.rx_pin": "RX pin",
  "uart.baud": "baud rate",
  "uart.detect_baud": "automatic speed setting",
  "led.type": "LED type",
  "led.pin": "LED pin",
};

// ---------------------------------------------------------------------------
// The page

export interface SettingsPageOptions {
  onBack(): void;
  /** The bridge gave this browser a new token (PIN on or changed), or took it away. */
  onToken(token: string | null): void;
  /** The bridge is about to close this session; `message` says why and what next. */
  onLeaving(message: string, willReconnect: boolean): void;
  /** The bridge accepted firmware `version` and is restarting into it. */
  onFirmwareInstalled(version: string): void;
  /** A line to show at the top of the page once, such as how an update went. */
  announce?: { tone: "good" | "bad"; text: string };
}

export interface SettingsPageHandles {
  root: HTMLElement;
  dispose(): void;
}

type SectionName = "status" | "dspi" | "wifi" | "security" | "bridge" | "maintenance";

type WifiStep = "idle" | "scanning" | "choosing" | "password";

type PinStep = "idle" | "turning_on" | "changing";

export function createSettingsPage(
  client: LinkClient,
  options: SettingsPageOptions,
): SettingsPageHandles {
  const page = {
    config: null as BridgeConfig | null,
    choices: null as BridgeOptions | null,
    stats: null as BridgeStats | null,
    loadError: null as string | null,
    // Unsaved edits; null when the section shows the saved configuration.
    uartDraft: null as UartConfig | null,
    bridgeDraft: null as Omit<BridgeConfig, "uart"> | null,
    // Wi-Fi
    wifiStep: "idle" as WifiStep,
    networks: [] as WifiNetwork[],
    joinSsid: "",
    joinSecure: true,
    joinPassword: "",
    // Security. The hello said whether the PIN was on; this page keeps it
    // current from then on, since only an admin (this page) can change it.
    pinOn: client.hub?.auth === "pin",
    pinStep: "idle" as PinStep,
    newPin: "",
    newPinConfirm: "",
    clients: [] as AuthClientInfo[],
    // Per section: a request in flight, and the last thing that went wrong.
    busy: new Set<SectionName>(),
    errors: new Map<SectionName, string>(),
    // Firmware update: the share of the image acknowledged, 0 to 1, or null
    // when no update is running.
    updateProgress: null as number | null,
    // Set once the bridge is restarting, so nothing more is sent.
    leaving: false,
  };

  // -- layout -------------------------------------------------------------

  const bodies = {} as Record<SectionName, HTMLElement>;
  function section(name: SectionName, title: string): HTMLElement {
    bodies[name] = el("div");
    return el(
      "section",
      { class: "panel", "aria-labelledby": `section-${name}` },
      el("h2", { id: `section-${name}`, text: title }),
      bodies[name],
    );
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
    section("status", "Status"),
    section("dspi", "DSPi connection"),
    section("wifi", "Wi-Fi"),
    section("security", "Security"),
    section("bridge", "Bridge"),
    section("maintenance", "Maintenance"),
  );

  const DRAW: Record<SectionName, () => void> = {
    status: drawStatus,
    dspi: drawDspi,
    wifi: drawWifi,
    security: drawSecurity,
    bridge: drawBridge,
    maintenance: drawMaintenance,
  };

  function draw(name: SectionName): void {
    clear(bodies[name]);
    const error = page.errors.get(name);
    if (error) {
      bodies[name].appendChild(statusLine("bad", error));
    }
    DRAW[name]();
  }

  function drawAll(): void {
    for (const name of Object.keys(DRAW) as SectionName[]) draw(name);
  }

  function add(name: SectionName, ...nodes: (HTMLElement | null)[]): void {
    for (const node of nodes) if (node) bodies[name].appendChild(node);
  }

  /**
   * Runs one request for a section: marks it busy, clears its old error,
   * records a new one if the request fails, and redraws it either way.
   * Resolves true when the request worked.
   */
  async function run(name: SectionName, work: () => Promise<void>): Promise<boolean> {
    if (page.leaving) return false;
    page.busy.add(name);
    page.errors.delete(name);
    draw(name);
    let worked = true;
    try {
      await work();
    } catch (error) {
      worked = false;
      page.errors.set(name, explain(error));
    } finally {
      page.busy.delete(name);
      if (!page.leaving) draw(name);
    }
    return worked;
  }

  // -- Status -------------------------------------------------------------

  function drawStatus(): void {
    if (options.announce) {
      add("status", statusLine(options.announce.tone, options.announce.text));
    }
    if (page.loadError) {
      add("status", statusLine("bad", "Couldn't read the bridge's settings.", page.loadError));
      return;
    }
    const stats = page.stats;
    const device = client.deviceList[0];
    const uart = stats?.uart;

    if (device && device.state === "online") {
      const firmware = device.fw
        ? `firmware ${device.fw}${device.fw_beta ? ` beta ${device.fw_beta}` : ""}`
        : null;
      add(
        "status",
        statusLine(
          "good",
          "DSPi connected",
          [`Serial ${device.serial}`, firmware].filter(Boolean).join(", "),
        ),
      );
    } else {
      add(
        "status",
        statusLine(
          "warn",
          "DSPi not answering",
          "Check the three wires, and that the UART control interface is on in DSPi Console.",
        ),
      );
    }
    if (uart && uart.live && !uart.notify_enable) {
      add(
        "status",
        statusLine(
          "warn",
          "DSPi notifications are off",
          "Turn them on in DSPi Console's UART settings so changes show up straight away.",
        ),
      );
    }

    const facts = el("dl", { class: "facts" });
    const fact = (term: string, value: string): void => {
      facts.append(el("dt", { text: term }), el("dd", { text: value }));
    };
    if (uart) {
      fact("Link to the DSPi", `${formatBaud(uart.baud)} baud, ${linkHealth(uart)}`);
    }
    if (stats?.wifi.ssid) {
      fact(
        "Wi-Fi",
        `${stats.wifi.ssid}, ${signalWords(stats.wifi.rssi)} signal (${stats.wifi.rssi} dBm)`,
      );
      fact("Address", stats.wifi.ip);
    }
    const version = client.hub?.hub.version;
    fact("Bridge firmware", [version, stats?.chip.toUpperCase()].filter(Boolean).join(" on "));
    if (stats) fact("Running for", formatDuration(stats.uptime_s));
    add("status", facts);
  }

  async function refreshStats(): Promise<void> {
    if (page.leaving || client.state !== "ready") return;
    try {
      const ok = await client.hubStats();
      page.stats = (ok.bridge as BridgeStats | undefined) ?? null;
      draw("status");
    } catch {
      // A missed refresh is not worth a message; the next one may work.
    }
  }

  // -- DSPi connection ----------------------------------------------------

  function drawDspi(): void {
    if (!page.config || !page.choices) return;
    const saved = page.config.uart;
    const choices = page.choices.uart;
    const draft = (): UartConfig => page.uartDraft ?? saved;
    const edit = (change: Partial<UartConfig>): void => {
      page.uartDraft = { ...draft(), ...change };
      refresh();
    };

    const diagram = el("div", { class: "wiring" });
    const hint = el("div", { role: "status" });
    const save = el("button", { type: "button", class: "primary", text: "Save" });
    const undo = el("button", { type: "button", text: "Undo" });
    const refresh = (): void => {
      const uart = draft();
      diagram.innerHTML = wiringDiagram(uart.tx_pin, uart.rx_pin);
      clear(hint);
      if (uart.tx_pin === uart.rx_pin) {
        hint.appendChild(statusLine("bad", "TX and RX must be different pins."));
      } else if (uart.baud < RECOMMENDED_MIN_BAUD) {
        hint.appendChild(
          statusLine(
            "warn",
            "This speed is slow",
            "The bridge works, but reads take longer and meters update less often. 1,000,000 is recommended.",
          ),
        );
      }
      const changed = page.uartDraft !== null && !sameJson(page.uartDraft, saved);
      save.disabled = !changed || uart.tx_pin === uart.rx_pin || page.busy.has("dspi");
      undo.hidden = !changed;
    };

    save.addEventListener("click", () => {
      const change = page.uartDraft;
      if (!change) return;
      void run("dspi", async () => {
        page.config = (await client.setHubConfig({ uart: change })) as unknown as BridgeConfig;
        page.uartDraft = null;
      });
    });
    undo.addEventListener("click", () => {
      page.uartDraft = null;
      draw("dspi");
    });

    add(
      "dspi",
      el("p", {
        class: "dim",
        text: "Which of the bridge's pins are wired to your DSPi, and how fast they talk.",
      }),
      diagram,
      el(
        "div",
        { class: "fields" },
        selectField(
          "uart-tx",
          "Bridge TX pin (to the DSPi's RX)",
          pinChoices(choices.tx_pin),
          draft().tx_pin,
          (value) => edit({ tx_pin: Number(value) }),
        ),
        selectField(
          "uart-rx",
          "Bridge RX pin (from the DSPi's TX)",
          pinChoices(choices.rx_pin),
          draft().rx_pin,
          (value) => edit({ rx_pin: Number(value) }),
        ),
      ),
      selectField(
        "uart-baud",
        "Baud rate",
        choices.baud.map((baud) => ({ value: baud, label: formatBaud(baud) })),
        draft().baud,
        (value) => edit({ baud: Number(value) }),
        "The speed of the link to your DSPi. It must match the DSPi's UART setting in DSPi Console.",
      ),
      checkboxField(
        "uart-detect",
        "Find the speed by itself if the DSPi doesn't answer",
        draft().detect_baud,
        (checked) => edit({ detect_baud: checked }),
      ),
      hint,
      el("p", {
        class: "dim small",
        text: "Saving reconnects to the DSPi, so it drops out for a moment.",
      }),
      el("div", { class: "row" }, save, undo),
    );
    refresh();
  }

  // -- Wi-Fi --------------------------------------------------------------

  function drawWifi(): void {
    const current = page.stats?.wifi;
    const busy = page.busy.has("wifi");
    add(
      "wifi",
      current?.ssid
        ? el("p", {}, "Connected to ", el("strong", { text: current.ssid }), ` at ${current.ip}.`)
        : el("p", { class: "dim", text: "Reading the current network…" }),
    );

    switch (page.wifiStep) {
      case "idle":
        add(
          "wifi",
          el("button", { type: "button", text: "Change network", onClick: () => scanNetworks() }),
        );
        return;

      case "scanning":
        add(
          "wifi",
          el("p", {
            class: "dim",
            role: "status",
            text: "Looking for networks… this takes a few seconds.",
          }),
        );
        return;

      case "choosing": {
        const list = el("ul", { class: "network-list" });
        for (const network of page.networks) {
          list.appendChild(
            el(
              "li",
              {},
              el(
                "button",
                {
                  type: "button",
                  onClick: () => {
                    page.joinSsid = network.ssid;
                    page.joinSecure = network.secure;
                    page.joinPassword = "";
                    page.wifiStep = "password";
                    draw("wifi");
                    document.getElementById("wifi-password")?.focus();
                  },
                },
                el("span", { class: "grow", text: network.ssid }),
                el("span", {
                  class: "dim small",
                  text: `${signalWords(network.rssi)}${network.secure ? "" : ", open"}`,
                }),
              ),
            ),
          );
        }
        add(
          "wifi",
          el("p", { class: "dim small", text: "Only 2.4 GHz networks are shown." }),
          page.networks.length ? list : el("p", { class: "dim", text: "No networks found." }),
          el(
            "div",
            { class: "row" },
            el("button", { type: "button", text: "Refresh", onClick: () => scanNetworks() }),
            el("button", { type: "button", text: "Cancel", onClick: () => cancelWifi() }),
          ),
        );
        return;
      }

      case "password": {
        const join = el("button", {
          type: "button",
          class: "primary",
          text: `Join "${page.joinSsid}"`,
          onClick: () => joinNetwork(),
        });
        const refresh = (): void => {
          join.disabled = busy || !passwordFits(page.joinPassword, page.joinSecure);
        };
        const field = el("input", {
          type: "password",
          id: "wifi-password",
          value: page.joinPassword,
          autocomplete: "off",
          maxLength: 64,
          onInput: (event: Event) => {
            page.joinPassword = (event.target as HTMLInputElement).value;
            refresh();
          },
        });
        add(
          "wifi",
          el(
            "div",
            { class: "field" },
            el("label", { for: "wifi-password", text: `Password for "${page.joinSsid}"` }),
            field,
            page.joinSecure
              ? null
              : el("p", {
                  class: "dim small",
                  text: "This network is open; leave the password empty.",
                }),
          ),
          el("p", {
            class: "dim small",
            text:
              "The bridge leaves its current network to join this one, which closes this page's connection. " +
              "If the join works, the page reconnects once this device is on the same network. If it doesn't, " +
              `the bridge goes back to "${current?.ssid ?? "its current network"}" within about 30 seconds.`,
          }),
          el(
            "div",
            { class: "row" },
            join,
            el("button", {
              type: "button",
              text: "Back",
              onClick: () => {
                page.wifiStep = "choosing"; // The list from the last scan
                page.errors.delete("wifi");
                draw("wifi");
              },
            }),
          ),
        );
        refresh();
        return;
      }
    }
  }

  async function scanNetworks(): Promise<void> {
    page.wifiStep = "scanning";
    const worked = await run("wifi", async () => {
      page.networks = (await client.scanWifi()).networks;
    });
    page.wifiStep = worked ? "choosing" : "idle";
    draw("wifi");
  }

  function cancelWifi(): void {
    page.wifiStep = "idle";
    page.errors.delete("wifi");
    draw("wifi");
  }

  async function joinNetwork(): Promise<void> {
    const ssid = page.joinSsid;
    const previous = page.stats?.wifi.ssid;
    await leaveAfter(
      "wifi",
      () => client.joinWifi(ssid, page.joinPassword),
      `The bridge is joining "${ssid}". This page reconnects once this device is on the same network. ` +
        (previous
          ? `If the bridge can't join, it goes back to "${previous}" within about 30 seconds.`
          : ""),
      true,
    );
  }

  // -- Security -----------------------------------------------------------

  function drawSecurity(): void {
    const switchOn = page.pinOn || page.pinStep === "turning_on";
    const toggle = el("input", {
      type: "checkbox",
      id: "pin-switch",
      role: "switch",
      checked: switchOn,
      disabled: page.busy.has("security"),
      "aria-describedby": "pin-switch-help",
      onChange: (event: Event) => {
        const on = (event.target as HTMLInputElement).checked;
        if (page.pinOn && !on) {
          void turnPinOff();
        } else {
          page.pinStep = on ? "turning_on" : "idle";
          page.newPin = "";
          page.newPinConfirm = "";
          draw("security");
          document.getElementById(on ? "new-pin" : "pin-switch")?.focus();
        }
      },
    });
    add(
      "security",
      el(
        "label",
        { class: "switch", for: "pin-switch" },
        el("span", { text: PIN_SWITCH_LABEL }),
        toggle,
      ),
      el("p", { class: "dim small", id: "pin-switch-help", text: PIN_SWITCH_HELP }),
    );

    if (page.pinStep === "turning_on") {
      add(
        "security",
        pinForm(
          "Turn on PIN",
          "Browsers and apps connected now, other than this one, are disconnected until they enter the PIN.",
        ),
      );
      return;
    }
    if (!page.pinOn) return;

    if (page.pinStep === "changing") {
      add(
        "security",
        pinForm("Save new PIN", "Every other browser and app will need to enter the new PIN."),
      );
    } else {
      add(
        "security",
        el("button", {
          type: "button",
          text: "Change PIN",
          onClick: () => {
            page.pinStep = "changing";
            page.newPin = "";
            page.newPinConfirm = "";
            draw("security");
            document.getElementById("new-pin")?.focus();
          },
        }),
      );
    }
    add("security", el("h3", { text: "Paired browsers and apps" }), clientList());
  }

  /** The new PIN, its confirmation, and the button that applies it. */
  function pinForm(submitLabel: string, consequence: string): HTMLElement {
    const submit = el("button", { type: "button", class: "primary", text: submitLabel });
    const hint = el("p", { class: "small", role: "status" });
    const refresh = (): void => {
      const mismatch =
        page.newPinConfirm.length === PIN_LENGTH && page.newPinConfirm !== page.newPin;
      hint.textContent = mismatch ? "The two PINs don't match." : "";
      submit.disabled =
        page.busy.has("security") || !pinIsValid(page.newPin) || page.newPinConfirm !== page.newPin;
    };
    submit.addEventListener("click", () => void setPin(page.newPin));
    const form = el(
      "div",
      { class: "pin-form" },
      pinInput("new-pin", "PIN (6 digits)", page.newPin, (value) => {
        page.newPin = value;
        refresh();
      }),
      pinInput("new-pin-confirm", "Enter the PIN again", page.newPinConfirm, (value) => {
        page.newPinConfirm = value;
        refresh();
      }),
      hint,
      el("p", { class: "small", text: PIN_REMINDER }),
      el("p", { class: "dim small", text: consequence }),
      el(
        "div",
        { class: "row" },
        submit,
        el("button", {
          type: "button",
          text: "Cancel",
          onClick: () => {
            page.pinStep = "idle";
            draw("security");
          },
        }),
      ),
    );
    refresh();
    return form;
  }

  async function setPin(pin: string): Promise<void> {
    const worked = await run("security", async () => {
      options.onToken(await client.setAuth("pin", pin));
      page.pinOn = true;
      page.pinStep = "idle";
      page.newPin = "";
      page.newPinConfirm = "";
      page.clients = await client.listClients();
    });
    if (worked) draw("security");
  }

  async function turnPinOff(): Promise<void> {
    const sure = await confirmAction({
      title: "Turn off the PIN?",
      body: [
        "Anyone on your network will be able to control your DSPi and change these settings.",
        "Every paired browser and app is forgotten. If you turn the PIN on again, each one enters it again.",
      ],
      confirmLabel: "Turn off PIN",
    });
    if (!sure) {
      draw("security"); // Put the switch back on.
      return;
    }
    await run("security", async () => {
      await client.setAuth("none");
      options.onToken(null);
      page.pinOn = false;
      page.pinStep = "idle";
      page.clients = [];
    });
  }

  function clientList(): HTMLElement {
    if (page.clients.length === 0) {
      return el("p", { class: "dim", text: "None yet." });
    }
    const list = el("ul", { class: "client-list" });
    for (const paired of page.clients) {
      const seen = paired.online
        ? "Connected now"
        : paired.last_seen
          ? `Last seen ${timeAgo(paired.last_seen)}`
          : "Not seen since pairing";
      list.appendChild(
        el(
          "li",
          {},
          el(
            "span",
            { class: "grow" },
            el("strong", { text: paired.name }),
            el("br"),
            el("span", { class: "dim small", text: seen }),
          ),
          el("button", {
            type: "button",
            text: "Remove",
            "aria-label": `Remove ${paired.name}`,
            disabled: page.busy.has("security"),
            onClick: () => void removeClient(paired),
          }),
        ),
      );
    }
    return list;
  }

  async function removeClient(paired: AuthClientInfo): Promise<void> {
    const sure = await confirmAction({
      title: `Remove ${paired.name}?`,
      body: [
        `${paired.name} will need the PIN to connect again. If it is connected now, it is disconnected.`,
        "If this is the browser you are using, this page disconnects too.",
      ],
      confirmLabel: "Remove",
      danger: true,
    });
    if (!sure) return;
    await run("security", async () => {
      await client.revokeClient(paired.cid);
      page.clients = await client.listClients();
    });
  }

  // -- Bridge -------------------------------------------------------------

  function drawBridge(): void {
    if (!page.config || !page.choices) return;
    const saved = { name: page.config.name, hostname: page.config.hostname, led: page.config.led };
    const draft = (): typeof saved => page.bridgeDraft ?? saved;
    const edit = (change: Partial<typeof saved>): void => {
      page.bridgeDraft = { ...draft(), ...change };
      refresh();
    };

    const hostnameHelp = el("p", { class: "dim small" });
    const ledPin = el("div");
    const hint = el("div", { role: "status" });
    const save = el("button", { type: "button", class: "primary", text: "Save" });
    const undo = el("button", { type: "button", text: "Undo" });
    const refresh = (): void => {
      const values = draft();
      hostnameHelp.textContent = `This bridge's address on your network: http://${values.hostname || "…"}.local:11915`;
      ledPin.hidden = values.led.type === "none";
      const problem = bridgeProblem(values);
      clear(hint);
      if (problem) hint.appendChild(statusLine("bad", problem));
      const changed = page.bridgeDraft !== null && !sameJson(page.bridgeDraft, saved);
      save.disabled = !changed || problem !== null || page.busy.has("bridge");
      undo.hidden = !changed;
    };

    save.addEventListener("click", () => {
      const change = page.bridgeDraft;
      if (!change) return;
      void run("bridge", async () => {
        page.config = (await client.setHubConfig(
          change as unknown as HubConfig,
        )) as unknown as BridgeConfig;
        page.bridgeDraft = null;
      });
    });
    undo.addEventListener("click", () => {
      page.bridgeDraft = null;
      draw("bridge");
    });

    const ledPins = page.choices.led.pin;
    ledPin.appendChild(
      selectField(
        "led-pin",
        "LED pin",
        pinChoices(ledPins),
        draft().led.pin ?? ledPins[0],
        (value) => edit({ led: { ...draft().led, pin: Number(value) } }),
      ),
    );

    const hostnameField = textField("bridge-hostname", "Hostname", draft().hostname, 63, (value) =>
      edit({ hostname: value.toLowerCase() }),
    );
    hostnameField.appendChild(hostnameHelp);

    add(
      "bridge",
      textField(
        "bridge-name",
        "Name",
        draft().name,
        63,
        (value) => edit({ name: value }),
        "DSPi Console and other apps show this name.",
      ),
      hostnameField,
      selectField(
        "led-type",
        "Status LED",
        page.choices.led.type.map((type) => ({
          value: type,
          label: LED_TYPE_LABELS[type] ?? type,
        })),
        draft().led.type,
        (value) => {
          const type = value as LedType;
          edit({ led: { type, pin: type === "none" ? null : (draft().led.pin ?? ledPins[0]) } });
        },
        "A light on the bridge that shows how it is doing.",
      ),
      ledPin,
      hint,
      el("div", { class: "row" }, save, undo),
    );
    refresh();
  }

  /** Why the Bridge section's values cannot be saved, or null when they can. */
  function bridgeProblem(values: {
    name: string;
    hostname: string;
    led: LedConfig;
  }): string | null {
    const nameBytes = new TextEncoder().encode(values.name.trim()).length;
    if (nameBytes === 0) return "Give the bridge a name.";
    if (nameBytes > 63) return "The name is too long.";
    if (!/^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$/.test(values.hostname)) {
      return "A hostname uses only lower-case letters, digits and hyphens, and cannot start or end with a hyphen.";
    }
    const uart = page.config?.uart;
    if (
      values.led.pin !== null &&
      uart &&
      (values.led.pin === uart.tx_pin || values.led.pin === uart.rx_pin)
    ) {
      return "The LED cannot use a pin wired to the DSPi.";
    }
    return null;
  }

  // -- Maintenance --------------------------------------------------------

  function drawMaintenance(): void {
    const ssid = page.stats?.wifi.ssid;
    const busy = page.busy.has("maintenance");
    add(
      "maintenance",
      client.hasCap("hub_update") ? updateRow(busy) : null,
      maintenanceRow(
        "Restart",
        "Restarts the bridge. Everything is kept, and this page reconnects in about 10 seconds.",
        el("button", {
          type: "button",
          text: "Restart",
          disabled: busy,
          onClick: () => void restart(),
        }),
      ),
      maintenanceRow(
        "Forget Wi-Fi",
        "Forgets the Wi-Fi network, so the bridge can be set up on another one. Everything else is kept.",
        el("button", {
          type: "button",
          class: "danger",
          text: "Forget Wi-Fi",
          disabled: busy,
          onClick: () => void forgetWifi(ssid),
        }),
      ),
      maintenanceRow(
        "Factory reset",
        "Erases every setting on the bridge. Your DSPi's own settings are not touched.",
        el("button", {
          type: "button",
          class: "danger",
          text: "Factory reset",
          disabled: busy,
          onClick: () => void factoryReset(),
        }),
      ),
    );
  }

  /** "Update firmware": a file picker, then progress while it uploads. */
  function updateRow(busy: boolean): HTMLElement {
    if (page.updateProgress !== null) {
      const percent = Math.round(page.updateProgress * 100);
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
        const file = (event.target as HTMLInputElement).files?.[0];
        if (file) void installFirmware(file);
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

  /** Why `image` cannot go on this bridge, or null if it can. */
  function firmwareProblem(image: Uint8Array): string | null {
    const info = readFirmwareInfo(image);
    if (!info) {
      return "That file is not ESP32 firmware. Choose the .bin file made for the DSPi bridge.";
    }
    if (info.project !== "dspi_bridge") {
      return `That file is firmware for something else ("${info.project}"), not the DSPi bridge.`;
    }
    const chip = page.stats?.chip;
    if (chip && info.chip !== chip) {
      return `That file is for the ${chipLabel(info.chip)}, but this bridge is an ${chipLabel(chip)}.`;
    }
    return null;
  }

  async function installFirmware(file: File): Promise<void> {
    const image = new Uint8Array(await file.arrayBuffer());
    const problem = firmwareProblem(image);
    if (problem) {
      page.errors.set("maintenance", problem);
      draw("maintenance");
      return;
    }
    const version = readFirmwareInfo(image)!.version;
    const current = client.hub?.hub.version;
    const sure = await confirmAction({
      title: `Install firmware ${version}?`,
      body: [
        `${current ? `The bridge runs firmware ${current} now. ` : ""}Installing takes about a ` +
          "minute, and the bridge restarts at the end, which disconnects every app for a moment.",
        "Everything else is kept: Wi-Fi, settings, the PIN and paired browsers and apps.",
        "If the new firmware does not start properly, the bridge goes back to this one by itself.",
      ],
      confirmLabel: "Install",
    });
    if (!sure) return;

    page.updateProgress = 0;
    let installed = "";
    const worked = await run("maintenance", async () => {
      installed = await client.updateHub(image, {
        version,
        onProgress: (written, total) => {
          page.updateProgress = written / total;
          draw("maintenance");
        },
      });
    });
    page.updateProgress = null;
    if (worked) {
      page.leaving = true;
      options.onFirmwareInstalled(installed);
      options.onLeaving(
        `The bridge is restarting into firmware ${installed}. This page reconnects when it's ` +
          "back, in about 15 seconds.",
        true,
      );
    } else {
      draw("maintenance");
    }
  }

  async function restart(): Promise<void> {
    await leaveAfter(
      "maintenance",
      () => client.restartHub(),
      "The bridge is restarting. This page reconnects when it's back, in about 10 seconds.",
      true,
    );
  }

  async function forgetWifi(ssid: string | undefined): Promise<void> {
    const sure = await confirmAction({
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
    await leaveAfter(
      "maintenance",
      () => client.resetHub("network"),
      'The bridge has left your network. To set it up again, join the Wi-Fi network whose name starts with "DSPi Bridge" from a phone.',
      false,
    );
  }

  async function factoryReset(): Promise<void> {
    const sure = await confirmAction({
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
    await leaveAfter(
      "maintenance",
      () => client.resetHub("factory"),
      'The bridge has been reset. To set it up again, join the Wi-Fi network whose name starts with "DSPi Bridge" from a phone.',
      false,
    );
  }

  /**
   * Sends a request after which the bridge closes every session (Link spec
   * 7.11 and 7.13), and tells the control page what happens next.
   */
  async function leaveAfter(
    name: SectionName,
    request: () => Promise<void>,
    message: string,
    willReconnect: boolean,
  ): Promise<void> {
    const worked = await run(name, request);
    if (worked) {
      page.leaving = true;
      options.onLeaving(message, willReconnect);
    }
  }

  // -- start --------------------------------------------------------------

  async function load(): Promise<void> {
    try {
      const [{ config, options: choices }, stats, clients] = await Promise.all([
        client.getHubConfig(),
        client.hubStats(),
        page.pinOn ? client.listClients() : Promise.resolve([]),
      ]);
      page.config = config as unknown as BridgeConfig;
      page.choices = choices as unknown as BridgeOptions;
      page.stats = (stats.bridge as BridgeStats | undefined) ?? null;
      page.clients = clients;
    } catch (error) {
      page.loadError = explain(error);
    }
    drawAll();
  }

  const stopConfig = client.on("hubConfig", (config) => {
    // Another admin, or this page, changed something. Sections with unsaved
    // edits keep them; the others show the new values.
    page.config = config as unknown as BridgeConfig;
    if (!page.uartDraft) draw("dspi");
    if (!page.bridgeDraft) draw("bridge");
  });
  const stopDevices = client.on("devices", () => draw("status"));
  const timer = setInterval(() => void refreshStats(), STATS_INTERVAL_MS);

  drawAll();
  void load();

  return {
    root,
    dispose() {
      clearInterval(timer);
      stopConfig();
      stopDevices();
    },
  };
}

// ---------------------------------------------------------------------------
// Small pieces

type Tone = "good" | "warn" | "bad";

/** A status line: the words carry the meaning, the colour only helps (spec 10.4). */
function statusLine(tone: Tone, title: string, detail?: string): HTMLElement {
  return el(
    "div",
    { class: `status ${tone}`, role: tone === "bad" ? "alert" : null },
    el("strong", { text: title }),
    detail ? el("span", { class: "detail", text: detail }) : null,
  );
}

interface Choice {
  value: string | number;
  label: string;
}

function pinChoices(pins: number[]): Choice[] {
  return pins.map((pin) => ({ value: pin, label: `GPIO ${pin}` }));
}

function selectField(
  id: string,
  label: string,
  choices: Choice[],
  selected: string | number,
  onChange: (value: string) => void,
  help?: string,
): HTMLElement {
  const select = el("select", {
    id,
    onChange: (event: Event) => onChange((event.target as HTMLSelectElement).value),
  });
  for (const choice of choices) {
    select.appendChild(
      el("option", {
        value: String(choice.value),
        text: choice.label,
        selected: choice.value === selected,
      }),
    );
  }
  return el(
    "div",
    { class: "field" },
    el("label", { for: id, text: label }),
    select,
    help ? el("p", { class: "dim small", text: help }) : null,
  );
}

function textField(
  id: string,
  label: string,
  value: string,
  maxLength: number,
  onInput: (value: string) => void,
  help?: string,
): HTMLElement {
  return el(
    "div",
    { class: "field" },
    el("label", { for: id, text: label }),
    el("input", {
      type: "text",
      id,
      value,
      maxLength,
      autocapitalize: "off",
      spellcheck: false,
      onInput: (event: Event) => onInput((event.target as HTMLInputElement).value),
    }),
    help ? el("p", { class: "dim small", text: help }) : null,
  );
}

function checkboxField(
  id: string,
  label: string,
  checked: boolean,
  onChange: (checked: boolean) => void,
): HTMLElement {
  return el(
    "label",
    { class: "checkbox", for: id },
    el("input", {
      type: "checkbox",
      id,
      checked,
      onChange: (event: Event) => onChange((event.target as HTMLInputElement).checked),
    }),
    el("span", { text: label }),
  );
}

/** A 6-digit PIN field that holds only digits. */
function pinInput(
  id: string,
  label: string,
  value: string,
  onInput: (value: string) => void,
): HTMLElement {
  return el(
    "div",
    { class: "field" },
    el("label", { for: id, text: label }),
    el("input", {
      type: "password",
      id,
      class: "pin",
      value,
      inputMode: "numeric",
      pattern: "[0-9]*",
      autocomplete: "off",
      maxLength: PIN_LENGTH,
      onInput: (event: Event) => {
        const field = event.target as HTMLInputElement;
        field.value = digitsOnly(field.value);
        onInput(field.value);
      },
    }),
  );
}

function maintenanceRow(title: string, explanation: string, action: HTMLElement): HTMLElement {
  return el(
    "div",
    { class: "maintenance-row" },
    el(
      "div",
      { class: "grow" },
      el("strong", { text: title }),
      el("p", { class: "dim small", text: explanation }),
    ),
    action,
  );
}

/**
 * The three wires between the bridge and the DSPi, with the chosen pins. The
 * bridge's TX goes to the DSPi's RX and the other way round; that crossing is
 * the mistake people make most, so the drawing shows it.
 */
function wiringDiagram(txPin: number, rxPin: number): string {
  const rows: Array<[string, string, string]> = [
    [`GPIO ${txPin} (TX)`, `GPIO ${DSPI_RX_PIN} (RX)`, "Bridge sends"],
    [`GPIO ${rxPin} (RX)`, `GPIO ${DSPI_TX_PIN} (TX)`, "DSPi sends"],
    ["GND", "GND", "Ground"],
  ];
  const lines = rows
    .map(([left, right, wire], i) => {
      const y = 46 + i * 34;
      return (
        `<text x="18" y="${y + 4}" class="pin">${left}</text>` +
        `<text x="342" y="${y + 4}" class="pin" text-anchor="end">${right}</text>` +
        `<line x1="132" y1="${y}" x2="228" y2="${y}" class="wire wire-${i}"/>` +
        `<text x="180" y="${y - 6}" class="wire-label" text-anchor="middle">${wire}</text>`
      );
    })
    .join("");
  return (
    `<svg viewBox="0 0 360 150" role="img" aria-label="Bridge GPIO ${txPin} to DSPi RX, ` +
    `bridge GPIO ${rxPin} to DSPi TX, ground to ground">` +
    `<rect x="4" y="4" width="128" height="142" rx="10" class="board"/>` +
    `<rect x="228" y="4" width="128" height="142" rx="10" class="board"/>` +
    `<text x="68" y="24" class="board-label" text-anchor="middle">Bridge</text>` +
    `<text x="292" y="24" class="board-label" text-anchor="middle">DSPi</text>` +
    lines +
    `</svg>`
  );
}

/** A sentence the user can act on, from whatever a request threw. */
function explain(error: unknown): string {
  if (error instanceof LinkProtocolError) {
    if (error.field) {
      return `The bridge didn't accept the ${FIELD_LABELS[error.field] ?? error.field}.`;
    }
    switch (error.code) {
      case "denied":
        return "Only an admin can change the bridge's settings.";
      case "busy":
        return "The bridge is busy with another change. Try again in a moment.";
      case "rate_limited":
        return "Too many tries. Wait a minute and try again.";
    }
    return error.detail ?? error.message;
  }
  return error instanceof Error ? error.message : String(error);
}

/** True when two plain values hold the same data. */
function sameJson(a: unknown, b: unknown): boolean {
  return JSON.stringify(a) === JSON.stringify(b);
}

/** Empty for an open network; otherwise 8 to 64 characters, as WPA2 needs. */
function passwordFits(password: string, secure: boolean): boolean {
  return secure
    ? password.length >= 8 && password.length <= 64
    : password.length === 0 || password.length >= 8;
}

function formatBaud(baud: number): string {
  return baud.toLocaleString("en-US");
}

function signalWords(rssi: number): string {
  return rssi >= -55 ? "excellent" : rssi >= -65 ? "good" : rssi >= -75 ? "fair" : "weak";
}

function linkHealth(uart: BridgeStats["uart"]): string {
  const problems = uart.timeouts + uart.crc_errors;
  if (!uart.live) return "no answer";
  if (problems === 0) return "no errors";
  return `${count(uart.timeouts, "timeout")} and ${count(uart.crc_errors, "damaged reply", "damaged replies")} since starting`;
}

/** "1 timeout", "3 timeouts". */
function count(n: number, one: string, many = `${one}s`): string {
  return `${n} ${n === 1 ? one : many}`;
}

function formatDuration(seconds: number): string {
  const minutes = Math.floor(seconds / 60);
  const hours = Math.floor(minutes / 60);
  const days = Math.floor(hours / 24);
  if (days > 0) return `${days} d ${hours % 24} h`;
  if (hours > 0) return `${hours} h ${minutes % 60} min`;
  return `${minutes} min`;
}

/** "5 minutes ago", from an ISO 8601 time. */
function timeAgo(iso: string): string {
  const seconds = (Date.parse(iso) - Date.now()) / 1000;
  if (!Number.isFinite(seconds)) return "at an unknown time";
  const format = new Intl.RelativeTimeFormat("en", { numeric: "auto" });
  const units: Array<[Intl.RelativeTimeFormatUnit, number]> = [
    ["day", 86400],
    ["hour", 3600],
    ["minute", 60],
  ];
  for (const [unit, size] of units) {
    if (Math.abs(seconds) >= size) return format.format(Math.round(seconds / size), unit);
  }
  return "just now";
}
