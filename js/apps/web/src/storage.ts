/*
 * What the control page remembers in localStorage: the last hub address, this
 * browser's name, and one pairing token per hub. Tokens are keyed on the hub
 * id, never the address, which changes (Link spec 6.2).
 *
 * Storage can be missing or refuse writes (private browsing, blocked site
 * data). The page then works without remembering anything, so every failure
 * here is deliberately ignored.
 */

const HOST_KEY = "dspi-link.host";
const CLIENT_NAME_KEY = "dspi-link.client_name";
const tokenKey = (hubId: string): string => `dspi-link.token.${hubId}`;

function read(key: string): string | null {
  try {
    return localStorage.getItem(key);
  } catch {
    return null; // No storage: behave as if nothing was saved.
  }
}

function write(key: string, value: string): void {
  try {
    localStorage.setItem(key, value);
  } catch {
    // No storage: the session still works, it just will not be remembered.
  }
}

function remove(key: string): void {
  try {
    localStorage.removeItem(key);
  } catch {
    // No storage: there is nothing to remove.
  }
}

/** @returns The hub address typed last, or null. */
export function loadHost(): string | null {
  return read(HOST_KEY);
}

/** @param host The address to offer next time. */
export function saveHost(host: string): void {
  write(HOST_KEY, host);
}

/** @returns The name this browser gave itself last, or null. */
export function loadClientName(): string | null {
  return read(CLIENT_NAME_KEY);
}

/** @param name The name to offer next time. */
export function saveClientName(name: string): void {
  write(CLIENT_NAME_KEY, name);
}

/**
 * @param hubId The hub's id from its hello or info reply.
 * @returns The token this browser holds for that hub, or null.
 */
export function loadToken(hubId: string): string | null {
  return read(tokenKey(hubId));
}

/**
 * @param hubId The hub's id.
 * @param token The token it issued.
 */
export function saveToken(hubId: string, token: string): void {
  write(tokenKey(hubId), token);
}

/** @param hubId The hub whose token is no longer good. */
export function forgetToken(hubId: string): void {
  remove(tokenKey(hubId));
}
