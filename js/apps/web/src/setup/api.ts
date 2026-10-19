/*
 * The bridge's setup API (bridge spec 9.3): plain HTTP and JSON under
 * /setup/v1/, answered only on the setup network. The setup page talks to
 * the bridge only through these functions, and every one gives up after a
 * few seconds, since the setup network drops while the bridge joins.
 */

import type { WifiNetwork } from "@dspi/link";

/** The reply to GET /setup/v1/state. */
export interface SetupState {
  name: string;
  chip: string;
  version: string;
  pin_required: boolean;
  dspi: { found: boolean; serial?: string; baud?: number; notify_enable?: boolean };
  wifi: {
    saved_ssid: string | null;
    join: "idle" | "connecting" | "connected" | "failed";
    failure?: "wrong_password" | "not_found" | "timed_out";
    ip: string | null;
    hostname: string;
    setup_ssid: string;
  };
}

/** One network from GET /setup/v1/networks; the same shape as Link's hub.wifi.scan. */
export type Network = WifiNetwork;

/** An error the bridge explained, with a sentence fit to show the user. */
export class SetupError extends Error {
  /** @param message The sentence. */
  constructor(message: string) {
    super(message);
    this.name = "SetupError";
  }
}

/** Longer than a scan takes (a few seconds), shorter than a user's patience. */
const REQUEST_TIMEOUT_MS = 8000;

const NOT_ANSWERING = "The bridge isn't answering. Check this device is still on its network.";

/** Something other than the bridge's setup API answered, such as a router's page. */
const NOT_THE_BRIDGE =
  "Something other than the bridge answered. Check this device is on its network.";

async function request(path: string, init?: RequestInit): Promise<Record<string, unknown>> {
  // AbortSignal.timeout would do, but Safari before 16 lacks it.
  const timeout = new AbortController();
  const timer = setTimeout(() => timeout.abort(), REQUEST_TIMEOUT_MS);
  try {
    let response: Response;
    try {
      response = await fetch(`/setup/v1/${path}`, {
        cache: "no-store",
        signal: timeout.signal,
        ...init,
      });
    } catch {
      throw new SetupError(NOT_ANSWERING);
    }
    const body: unknown = await response.json().catch(() => null);
    const object = isObject(body) ? body : null;
    if (!response.ok) {
      const msg = typeof object?.msg === "string" ? object.msg : null;
      throw new SetupError(msg ?? `The bridge refused the request (error ${response.status}).`);
    }
    if (!object) throw new SetupError(NOT_THE_BRIDGE);
    return object;
  } finally {
    clearTimeout(timer);
  }
}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

/** The reply to /state, checked: everything the page reads from it must be there. */
function readState(body: Record<string, unknown>): SetupState {
  const { dspi, wifi } = body;
  const fine =
    typeof body.name === "string" &&
    typeof body.pin_required === "boolean" &&
    isObject(dspi) &&
    typeof dspi.found === "boolean" &&
    isObject(wifi) &&
    typeof wifi.join === "string" &&
    typeof wifi.hostname === "string";
  if (!fine) throw new SetupError(NOT_THE_BRIDGE);
  return body as unknown as SetupState;
}

/**
 * @returns What the bridge knows now: its name, the DSPi and the Wi-Fi join.
 * @throws SetupError when the bridge does not answer, refuses, or sends
 *   something that is not its state.
 */
export async function getState(): Promise<SetupState> {
  return readState(await request("state"));
}

/**
 * @returns The networks the bridge can see, strongest first.
 * @throws SetupError when the bridge does not answer or refuses.
 */
export async function getNetworks(): Promise<Network[]> {
  const { networks } = await request("networks");
  if (!Array.isArray(networks)) return [];
  return networks.filter(
    (network): network is Network =>
      isObject(network) &&
      typeof network.ssid === "string" &&
      typeof network.rssi === "number" &&
      typeof network.secure === "boolean",
  );
}

/**
 * What `apply` does to the bridge's PIN. `pin_enabled` true sets `pin`; false
 * turns the PIN off; left out, the PIN stays as it is. `current_pin` is
 * needed whenever the bridge already has a PIN (bridge spec 9.3).
 */
export interface PinChange {
  pin_enabled?: boolean;
  pin?: string;
  current_pin?: string;
}

function postJson(path: string, body: unknown): Promise<Record<string, unknown>> {
  return request(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
}

/**
 * Saves the settings and starts the join; getState() then reports how it goes.
 *
 * @param ssid The home network.
 * @param password Its password; empty for an open network.
 * @param name The bridge's new name.
 * @param pin What to do to the PIN.
 * @throws SetupError with the bridge's reason, such as a wrong current PIN.
 */
export async function apply(
  ssid: string,
  password: string,
  name: string,
  pin: PinChange,
): Promise<void> {
  await postJson("apply", { ssid, password, name, ...pin });
}

/**
 * Tells the bridge setup is over, so it turns its setup network off.
 *
 * @param currentPin The bridge's PIN, if it has one now; empty otherwise.
 * @throws SetupError when the bridge does not answer or refuses.
 */
export async function finish(currentPin: string): Promise<void> {
  await postJson("finish", currentPin ? { current_pin: currentPin } : {});
}
