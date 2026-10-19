/*
 * Wi-Fi wording and rules, shared by the setup page and the settings page so
 * both judge a password and describe a signal the same way (bridge spec 10.2
 * and 10.3).
 */

/** The fewest bytes a WPA2 passphrase may have. */
const PASSPHRASE_MIN_BYTES = 8;
/** The most bytes a WPA2 passphrase may have; 64 means a raw key in hex. */
const PASSPHRASE_MAX_BYTES = 63;
/** A raw WPA2 key: 64 hexadecimal digits. */
const RAW_KEY = /^[0-9a-fA-F]{64}$/;

/** The longest password field the pages offer: a raw key. */
export const WIFI_PASSWORD_MAX_LENGTH = 64;

/** The longest network name Wi-Fi allows. */
export const SSID_MAX_BYTES = 32;

/** Signal levels in dBm at which the strength rises a step. */
const EXCELLENT_RSSI_DBM = -55;
const GOOD_RSSI_DBM = -65;
const FAIR_RSSI_DBM = -75;

/**
 * Whether a password suits the network: empty for an open network; 8 to 63
 * bytes, or a 64-digit hex key, for a secured one.
 *
 * @param password What the user typed.
 * @param secure Whether the network asks for a password.
 * @returns True when the bridge can try to join with it.
 */
export function passwordFits(password: string, secure: boolean): boolean {
  if (!secure) return password.length === 0;
  if (RAW_KEY.test(password)) return true;
  const bytes = new TextEncoder().encode(password).length;
  return bytes >= PASSPHRASE_MIN_BYTES && bytes <= PASSPHRASE_MAX_BYTES;
}

/**
 * Signal strength as 1 to 4 bars.
 *
 * @param rssi The signal level in dBm.
 * @returns 4 for excellent down to 1 for weak.
 */
export function signalBars(rssi: number): number {
  if (rssi >= EXCELLENT_RSSI_DBM) return 4;
  if (rssi >= GOOD_RSSI_DBM) return 3;
  if (rssi >= FAIR_RSSI_DBM) return 2;
  return 1;
}

/**
 * Signal strength in a word.
 *
 * @param rssi The signal level in dBm.
 * @returns "excellent", "good", "fair" or "weak".
 */
export function signalWords(rssi: number): string {
  return ["weak", "fair", "good", "excellent"][signalBars(rssi) - 1];
}

/**
 * The networks worth offering: those with a name, since a hidden network is
 * joined by typing its name.
 *
 * @param networks A scan's results.
 * @returns The networks that have an SSID.
 */
export function namedNetworks<T extends { ssid: string }>(networks: T[]): T[] {
  return networks.filter((network) => network.ssid.trim() !== "");
}
