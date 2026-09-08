/*
 * Discovery over HTTP, spec 3.2 and 3.3.
 *
 * DNS-SD is not reachable from a browser, so a web client resolves a hub the
 * other way the spec defines: take a bare host or `host:port` and ask
 * /dspi/v1/info, which every hub answers with CORS open.
 */

import type { AuthMode, HubDescriptor, ProtoVersion } from "./messages.js";

export const LINK_DEFAULT_PORT = 11915;

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

/** Split a user-typed `host`, `host:port` or full URL into its parts. */
export function parseHostInput(input: string, defaultSecure = false): { host: string; port: number; secure: boolean } {
  let text = input.trim();
  let secure = defaultSecure;
  const scheme = /^(https?|wss?):\/\//i.exec(text);
  if (scheme) {
    secure = /^(https|wss)/i.test(scheme[1]);
    text = text.slice(scheme[0].length);
  }
  text = text.replace(/\/.*$/, "");
  // Bracketed IPv6 literal, e.g. [fe80::1]:11915
  const v6 = /^\[([^\]]+)\](?::(\d+))?$/.exec(text);
  if (v6) {
    return { host: `[${v6[1]}]`, port: v6[2] ? Number(v6[2]) : LINK_DEFAULT_PORT, secure };
  }
  const idx = text.lastIndexOf(":");
  if (idx > 0) {
    const maybePort = Number(text.slice(idx + 1));
    if (Number.isInteger(maybePort) && maybePort > 0) {
      return { host: text.slice(0, idx), port: maybePort, secure };
    }
  }
  return { host: text, port: secure ? 443 : LINK_DEFAULT_PORT, secure };
}

export function infoUrl(input: string, defaultSecure = false): string {
  const { host, port, secure } = parseHostInput(input, defaultSecure);
  return `${secure ? "https" : "http"}://${host}:${port}/dspi/v1/info`;
}

/** WebSocket URL for a hub, honouring the info reply's `tls` and `ws` fields. */
export function webSocketUrl(input: string, info?: HubInfo, defaultSecure = false): string {
  const { host, port, secure } = parseHostInput(input, defaultSecure);
  const useTls = info?.tls ?? secure;
  const path = info?.ws ?? "/dspi/v1";
  return `${useTls ? "wss" : "ws"}://${host}:${port}${path}`;
}

/** GET /dspi/v1/info. Rejects on a non-200 or a reply that is not hub info. */
export async function fetchHubInfo(input: string, defaultSecure = false, signal?: AbortSignal): Promise<HubInfo> {
  const res = await fetch(infoUrl(input, defaultSecure), { signal });
  if (!res.ok) throw new Error(`hub info: HTTP ${res.status}`);
  const body = (await res.json()) as HubInfo;
  if (!body || typeof body !== "object" || !body.hub) throw new Error("hub info: not a DSPi Link hub");
  return body;
}
