/*
 * dspi-link: a TypeScript client for the DSPi Link protocol (spec 1.2).
 *
 * Four layers, each usable on its own:
 *   frames.ts    binary data plane, spec 8
 *   messages.ts  JSON control plane, spec 7
 *   client.ts    a session: handshake, correlation, inventory, reconnect
 *   decoders.ts  the firmware payloads Link carries verbatim
 *   sha256.ts    SHA-256 for firmware uploads, on any origin
 */

export * from "./frames.js";
export * from "./messages.js";
export * from "./decoders.js";
export * from "./client.js";
export { sha256, sha256Hex } from "./sha256.js";
export {
  type HubInfo,
  fetchHubInfo,
  infoUrl,
  parseHostInput,
  webSocketUrl,
} from "./discovery.js";
