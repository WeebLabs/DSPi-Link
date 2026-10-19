/*
 * Discovery over HTTP, Link spec 3.2 and 3.3.
 *
 * DNS-SD is not reachable from a browser, so a web client resolves a hub the
 * other way the spec defines: take a bare host or `host:port` and ask
 * /dspi/v1/info, which every hub answers with CORS open.
 */

import { LINK_DEFAULT_PORT, LINK_INFO_PATH, LINK_WS_PATH } from "./constants.js";
import type { AuthMode, HubDescriptor, ProtoVersion } from "./messages.js";

/** The reply to GET /dspi/v1/info (Link spec 3.2). */
export interface HubInfo {
  proto: ProtoVersion;
  hub: HubDescriptor;
  auth: AuthMode;
  tls?: boolean;
  /** WebSocket path; defaults to /dspi/v1. */
  ws?: string;
  devices?: Array<{ serial: string; name?: string }>;
  [key: string]: unknown;
}

/** The port a secure address with no port means. */
const HTTPS_DEFAULT_PORT = 443;

/** How long fetchHubInfo waits when the caller gives no signal of its own. */
const HUB_INFO_TIMEOUT_MS = 5000;

/**
 * Splits a user-typed `host`, `host:port` or full URL into its parts.
 *
 * @param input What the user typed.
 * @param defaultSecure Whether to assume TLS when no scheme is given.
 * @returns The host (IPv6 addresses in brackets), the port and whether to use TLS.
 */
export function parseHostInput(
  input: string,
  defaultSecure = false,
): { host: string; port: number; secure: boolean } {
  let text = input.trim();
  let secure = defaultSecure;
  const scheme = /^(https?|wss?):\/\//i.exec(text);
  if (scheme) {
    secure = /^(https|wss)/i.test(scheme[1]);
    text = text.slice(scheme[0].length);
  }
  text = text.replace(/\/.*$/, "");
  // A secure page with no port is most likely behind a TLS proxy on 443.
  const defaultPort = secure ? HTTPS_DEFAULT_PORT : LINK_DEFAULT_PORT;
  // Bracketed IPv6 literal, e.g. [fe80::1]:11915
  const bracketed = /^\[([^\]]+)\](?::(\d+))?$/.exec(text);
  if (bracketed) {
    const port = bracketed[2] ? Number(bracketed[2]) : defaultPort;
    return { host: `[${bracketed[1]}]`, port, secure };
  }
  // More than one colon and no brackets: a bare IPv6 address, which cannot carry a port.
  if (text.indexOf(":") !== text.lastIndexOf(":")) {
    return { host: `[${text}]`, port: defaultPort, secure };
  }
  const colon = text.lastIndexOf(":");
  if (colon > 0) {
    const port = Number(text.slice(colon + 1));
    if (Number.isInteger(port) && port > 0) {
      return { host: text.slice(0, colon), port, secure };
    }
  }
  return { host: text, port: defaultPort, secure };
}

/**
 * The info endpoint for what the user typed.
 *
 * @param input A host, `host:port` or URL.
 * @param defaultSecure Whether to assume TLS when no scheme is given.
 * @returns The http(s) URL of /dspi/v1/info.
 */
export function infoUrl(input: string, defaultSecure = false): string {
  const { host, port, secure } = parseHostInput(input, defaultSecure);
  return `${secure ? "https" : "http"}://${host}:${port}${LINK_INFO_PATH}`;
}

/**
 * The WebSocket URL for a hub, honouring the info reply's `tls` and `ws` fields.
 *
 * @param input A host, `host:port` or URL.
 * @param info The hub's info reply, if it has been fetched.
 * @param defaultSecure Whether to assume TLS when neither says.
 * @returns The ws(s) URL to pass to LinkClient.connect.
 */
export function webSocketUrl(input: string, info?: HubInfo, defaultSecure = false): string {
  const { host, port, secure } = parseHostInput(input, defaultSecure);
  const useTls = info?.tls ?? secure;
  const path = info?.ws ?? LINK_WS_PATH;
  return `${useTls ? "wss" : "ws"}://${host}:${port}${path}`;
}

/**
 * Fetches GET /dspi/v1/info.
 *
 * @param input A host, `host:port` or URL.
 * @param defaultSecure Whether to assume TLS when no scheme is given.
 * @param signal Aborts the request; without one it gives up after 5 s.
 * @returns The hub's info. Rejects on a timeout, a non-200, or a reply that
 *   is not hub info.
 */
export async function fetchHubInfo(
  input: string,
  defaultSecure = false,
  signal?: AbortSignal,
): Promise<HubInfo> {
  // AbortSignal.timeout would do, but Safari before 16 lacks it.
  const timeout = signal ? null : new AbortController();
  const timer = timeout
    ? setTimeout(() => timeout.abort(new Error("hub info timed out")), HUB_INFO_TIMEOUT_MS)
    : null;
  let body: HubInfo;
  try {
    const response = await fetch(infoUrl(input, defaultSecure), {
      signal: signal ?? timeout?.signal,
    });
    if (!response.ok) throw new Error(`hub info: HTTP ${response.status}`);
    body = (await response.json()) as HubInfo;
  } finally {
    if (timer !== null) clearTimeout(timer);
  }
  if (!body || typeof body !== "object" || !body.hub) {
    throw new Error("hub info: not a DSPi Link hub");
  }
  return body;
}
