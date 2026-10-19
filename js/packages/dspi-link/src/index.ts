/*
 * dspi-link: a TypeScript client for the DSPi Link protocol (Link spec 1.2).
 *
 * The package's one entry point. Each module is usable on its own:
 *   constants.ts  the port, paths and versions, defined once
 *   frames.ts     binary data plane, Link spec 8
 *   messages.ts   JSON control plane, Link spec 7
 *   errors.ts     the errors the client rejects with
 *   client.ts     a session: handshake, correlation, inventory, reconnect
 *   hubUpdate.ts  hub firmware upload, Link spec 7.14
 *   discovery.ts  finding a hub over HTTP, Link spec 3.2 and 3.3
 *   decoders.ts   the firmware payloads Link carries verbatim
 *   base64.ts     the `_b64` fields and tokens
 *   sha256.ts     SHA-256 for firmware uploads, on any origin
 */

export * from "./constants.js";
export * from "./frames.js";
export * from "./messages.js";
export { LinkCommandError, LinkProtocolError } from "./errors.js";
export * from "./decoders.js";
export * from "./base64.js";
export * from "./client.js";
export { sha256, sha256Hex } from "./sha256.js";
export {
  type HubInfo,
  fetchHubInfo,
  infoUrl,
  parseHostInput,
  webSocketUrl,
} from "./discovery.js";
