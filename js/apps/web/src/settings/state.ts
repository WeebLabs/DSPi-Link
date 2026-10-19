/*
 * What the bridge settings page knows, and what each of its sections may use.
 *
 * The page (page.ts) owns one SettingsState and hands every section a
 * SectionContext; the sections (status.ts, dspi.ts, wifi.ts, security.ts,
 * bridge.ts, maintenance.ts) read and change the state only through it. The
 * bridge's configuration and stats arrive as untyped JSON (Link spec 7.9 and
 * 7.10, bridge spec 8.7 and 8.11), so they are checked here before any section
 * sees them.
 */

import type { AuthClientInfo, LinkClient, WifiNetwork } from "@dspi/link";
import type { ConfirmOptions } from "../dialog.js";

/** The `uart` object of the bridge's configuration. */
export interface UartConfig {
  tx_pin: number;
  rx_pin: number;
  baud: number;
  detect_baud: boolean;
}

/** The status LED's kinds (bridge spec 13). */
export type LedType = "none" | "gpio" | "gpio_active_low" | "ws2812";

/** The `led` object of the bridge's configuration. */
export interface LedConfig {
  type: LedType;
  pin: number | null;
}

/** The keys of the Bridge section: everything but the UART. */
export type BridgeValues = Omit<BridgeConfig, "uart">;

/** The bridge's configuration, from hub.config.get. */
export interface BridgeConfig {
  name: string;
  hostname: string;
  uart: UartConfig;
  led: LedConfig;
}

/** The values the bridge accepts for its fixed-set keys. */
export interface BridgeOptions {
  uart: { tx_pin: number[]; rx_pin: number[]; baud: number[] };
  led: { type: LedType[]; pin: number[] };
}

/** The `bridge` object of hub.stats. */
export interface BridgeStats {
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

/** The page's sections, in the order they appear. */
export type SectionName = "status" | "dspi" | "wifi" | "security" | "bridge" | "maintenance";

/** Where the Wi-Fi section is in moving the bridge to another network. */
export type WifiStep = "idle" | "scanning" | "choosing" | "password";

/** Where the Security section is in changing the PIN. */
export type PinStep = "idle" | "turning_on" | "changing";

/** Everything the page knows; see page.ts. */
export interface SettingsState {
  config: BridgeConfig | null;
  choices: BridgeOptions | null;
  stats: BridgeStats | null;
  loadError: string | null;
  /** Unsaved edits, only of the keys the user changed; null when the section
   * shows the saved configuration. Saving sends only these keys, so a change
   * another admin made meanwhile to any other key is kept (Link spec 7.10). */
  uartDraft: Partial<UartConfig> | null;
  bridgeDraft: Partial<BridgeValues> | null;
  /** The bridge's address after a hostname change this page saved. */
  movedTo: string | null;
  wifiStep: WifiStep;
  networks: WifiNetwork[];
  joinSsid: string;
  joinSecure: boolean;
  joinPassword: string;
  /** The hello said whether the PIN was on; this page keeps it current from
   * then on, since only an admin (this page) can change it. */
  pinOn: boolean;
  pinStep: PinStep;
  newPin: string;
  newPinConfirm: string;
  clients: AuthClientInfo[];
  /** Per section: a request in flight. */
  busy: Set<SectionName>;
  /** Per section: the last thing that went wrong. */
  errors: Map<SectionName, string>;
  /** Errors not yet drawn, which are read out once when they are. */
  freshErrors: Set<SectionName>;
  /** The share of a firmware image acknowledged, 0 to 1, or null when no update runs. */
  updateProgress: number | null;
  /** Set once the bridge is restarting, so nothing more is sent. */
  leaving: boolean;
  /** Set when the page is closed, so late answers change nothing. */
  disposed: boolean;
}

/** What the settings page's caller provides. */
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

/** What a section may do; provided by page.ts. */
export interface SectionContext {
  client: LinkClient;
  state: SettingsState;
  options: SettingsPageOptions;
  /** Appends nodes to a section's body; nulls are skipped. */
  add(name: SectionName, ...nodes: (HTMLElement | null)[]): void;
  /** Redraws one section from the state. */
  draw(name: SectionName): void;
  /**
   * Runs one request for a section: marks it busy, records an error if it
   * fails, and redraws it either way. Resolves true when it worked.
   */
  run(name: SectionName, work: () => Promise<void>): Promise<boolean>;
  /**
   * Runs a request after which the bridge closes every session (Link spec
   * 7.11, 7.13 and 7.14), then tells the control page what happens next.
   */
  leaveAfter(
    name: SectionName,
    request: () => Promise<void>,
    message: string,
    willReconnect: boolean,
  ): Promise<void>;
  /** Asks the user to confirm; resolves false if the page closes first. */
  confirm(options: ConfirmOptions): Promise<boolean>;
}

/** The empty state a page starts from. */
export function initialState(client: LinkClient): SettingsState {
  return {
    config: null,
    choices: null,
    stats: null,
    loadError: null,
    uartDraft: null,
    bridgeDraft: null,
    movedTo: null,
    wifiStep: "idle",
    networks: [],
    joinSsid: "",
    joinSecure: true,
    joinPassword: "",
    pinOn: client.hub?.auth === "pin",
    pinStep: "idle",
    newPin: "",
    newPinConfirm: "",
    clients: [],
    busy: new Set(),
    errors: new Map(),
    freshErrors: new Set(),
    updateProgress: null,
    leaving: false,
    disposed: false,
  };
}

// ---------------------------------------------------------------------------
// Checking what the bridge sent

type Json = Record<string, unknown>;

const LED_TYPES: readonly LedType[] = ["none", "gpio", "gpio_active_low", "ws2812"];

function isObject(value: unknown): value is Json {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function isWholeNumber(value: unknown): value is number {
  return typeof value === "number" && Number.isInteger(value) && value >= 0;
}

function isWholeNumberList(value: unknown): value is number[] {
  return Array.isArray(value) && value.every(isWholeNumber);
}

/**
 * Checks hub.config.get's or hub.config.set's `config`.
 *
 * @param value The `config` object as received.
 * @returns The configuration, or null if it is not a bridge's.
 */
export function readBridgeConfig(value: unknown): BridgeConfig | null {
  if (!isObject(value) || !isObject(value.uart) || !isObject(value.led)) return null;
  const { name, hostname, uart, led } = value;
  if (typeof name !== "string" || typeof hostname !== "string") return null;
  if (!isWholeNumber(uart.tx_pin) || !isWholeNumber(uart.rx_pin) || !isWholeNumber(uart.baud))
    return null;
  if (typeof uart.detect_baud !== "boolean") return null;
  if (!LED_TYPES.includes(led.type as LedType)) return null;
  if (led.pin !== null && !isWholeNumber(led.pin)) return null;
  return {
    name,
    hostname,
    uart: {
      tx_pin: uart.tx_pin,
      rx_pin: uart.rx_pin,
      baud: uart.baud,
      detect_baud: uart.detect_baud,
    },
    led: { type: led.type as LedType, pin: led.pin },
  };
}

/**
 * Checks hub.config.get's `options`.
 *
 * @param value The `options` object as received.
 * @returns The permitted values, or null if they are not a bridge's.
 */
export function readBridgeOptions(value: unknown): BridgeOptions | null {
  if (!isObject(value) || !isObject(value.uart) || !isObject(value.led)) return null;
  const { uart, led } = value;
  if (
    !isWholeNumberList(uart.tx_pin) ||
    !isWholeNumberList(uart.rx_pin) ||
    !isWholeNumberList(uart.baud)
  )
    return null;
  if (!isWholeNumberList(led.pin) || !Array.isArray(led.type)) return null;
  const types = led.type.filter((type): type is LedType => LED_TYPES.includes(type as LedType));
  return {
    uart: { tx_pin: uart.tx_pin, rx_pin: uart.rx_pin, baud: uart.baud },
    led: { type: types, pin: led.pin },
  };
}

/**
 * Checks hub.stats's `bridge` object. Only its shape is checked; every value
 * is shown as text, never as markup.
 *
 * @param value The `bridge` object as received.
 * @returns The stats, or null if there are none or they are not a bridge's.
 */
export function readBridgeStats(value: unknown): BridgeStats | null {
  if (!isObject(value) || !isObject(value.wifi) || !isObject(value.uart)) return null;
  if (typeof value.chip !== "string" || typeof value.uptime_s !== "number") return null;
  return value as unknown as BridgeStats;
}
