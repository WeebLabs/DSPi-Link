/*
 * The bridge's setup API (bridge spec 9.3): plain HTTP and JSON under
 * /setup/v1/, answered only on the setup network.
 */

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

export interface Network {
  ssid: string;
  rssi: number;
  secure: boolean;
}

/** An error the bridge explained, with a sentence fit to show the user. */
export class SetupError extends Error {}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  let response: Response;
  try {
    response = await fetch(`/setup/v1/${path}`, { cache: "no-store", ...init });
  } catch {
    throw new SetupError("The bridge isn't answering. Check this device is still on its network.");
  }
  const body = (await response.json().catch(() => ({}))) as T & { msg?: string };
  if (!response.ok) {
    throw new SetupError(body.msg ?? `The bridge answered ${response.status}.`);
  }
  return body;
}

export function getState(): Promise<SetupState> {
  return request<SetupState>("state");
}

export async function getNetworks(): Promise<Network[]> {
  const body = await request<{ networks: Network[] }>("networks");
  return body.networks;
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

function postJson<T>(path: string, body: unknown): Promise<T> {
  return request<T>(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
}

export async function apply(
  ssid: string,
  password: string,
  name: string,
  pin: PinChange,
): Promise<void> {
  await postJson("apply", { ssid, password, name, ...pin });
}

/** `currentPin` is the bridge's PIN, if it has one now. */
export async function finish(currentPin: string): Promise<void> {
  await postJson("finish", currentPin ? { current_pin: currentPin } : {});
}
