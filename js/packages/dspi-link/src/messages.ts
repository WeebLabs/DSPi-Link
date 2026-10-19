/*
 * Control-plane messages: the types and codec for Link spec 7 (7.1 to 7.14).
 *
 * Every text frame is one JSON object with a string `t`. Requests carry an
 * integer `id`; replies echo it as `t: "ok"` or `t: "err"`; events have no id.
 * Wire keys are snake_case and are written here exactly as they appear on the
 * wire, so the types double as the wire contract.
 *
 * Unknown fields are preserved on decode and unknown `t` values decode to a
 * bare envelope, because Link spec 10, rule 3, requires both sides to ignore
 * rather than reject what they do not recognise.
 */

export interface ProtoVersion {
  major: number;
  minor: number;
}

export type AuthMode = "none" | "pin";
/** Where a `pin` hub's PIN comes from, spec 6.2: shown by the hub for a
 * moment, or chosen by the user once. Added in 1.2. */
export type PinSource = "window" | "fixed";
export type HubKind = "console" | "bridge";
export type LinkRole = "viewer" | "control" | "admin";
export type DeviceState = "online" | "offline" | "updating";
export type DeviceLink = "usb" | "uart";

/** Error codes, spec 7.1. */
export type LinkErrorCode =
  | "unauthenticated"
  | "denied"
  | "bad_request"
  | "unknown_type"
  | "no_device"
  | "locked"
  | "busy"
  | "rate_limited"
  | "unsupported"
  | "internal"
  | (string & {});

/** Hub capability names, spec 7.2. The list is open; treat unknown names as opaque. */
export type LinkCapability =
  | "cmd"
  | "notify"
  | "poll"
  | "snapshot"
  | "lock"
  | "fw_install"
  | "web"
  | "rename"
  // 1.2: hub administration, spec 7.10 to 7.14
  | "hub_config"
  | "wifi"
  | "auth_config"
  | "reset"
  | "hub_update"
  | (string & {});

export interface HubDescriptor {
  id: string;
  name: string;
  kind: HubKind;
  version?: string;
}

export interface HubLimits {
  max_frame: number;
  max_payload: number;
  max_inflight: number;
  poll_max_hz: number;
  poll_budget_bps: number;
}

/** `[bRequest, dir]` pairs the session may not issue; dir 0 = SET, 1 = GET. */
export interface PolicyDescriptor {
  denied: Array<[number, number]>;
}

// ---------------------------------------------------------------------------
// Envelope and hello

export interface Envelope {
  t: string;
  id?: number;
}

export interface HelloClientMessage extends Envelope {
  t: "hello";
  proto: ProtoVersion;
  client: { name: string; app?: string; version?: string };
}

export interface HelloHubMessage extends Envelope {
  t: "hello";
  proto: ProtoVersion;
  hub: HubDescriptor;
  auth: AuthMode;
  /** Present only when `auth` is `pin`; absent means `window` (spec 7.2). */
  pin_source?: PinSource;
  caps: LinkCapability[];
  limits: HubLimits;
}

export type HelloMessage = HelloClientMessage | HelloHubMessage;

/**
 * Tells the two hellos apart: a hub hello carries `hub`, a client hello `client`.
 *
 * @param message A decoded hello.
 * @returns True for the hub's hello.
 */
export function isHubHello(message: HelloMessage): message is HelloHubMessage {
  return (message as HelloHubMessage).hub !== undefined;
}

/**
 * The hello's PIN source, with the spec's default for a hub that omits it.
 *
 * @param hello The hub's hello.
 * @returns "fixed" or "window" (Link spec 6.2).
 */
export function pinSourceOf(hello: HelloHubMessage): PinSource {
  return hello.pin_source ?? "window";
}

// ---------------------------------------------------------------------------
// Replies

/**
 * `ok` is the reply to every request type, so its body is the union of the
 * per-request reply bodies. Read only the fields the request you sent defines.
 */
export interface OkMessage extends Envelope {
  t: "ok";
  id: number;
  // auth.token / auth.pair
  session?: number;
  role?: LinkRole;
  token?: string;
  policy?: PolicyDescriptor;
  // device.list
  devices?: DeviceInfo[];
  // device.snapshot
  handle?: number;
  wire_version?: number;
  age_ms?: number;
  bulk_b64?: string;
  status_b64?: string;
  // poll.subscribe
  granted?: PollGrant[];
  // auth.list
  clients?: AuthClientInfo[];
  // fw.install / hub.update
  xfer?: number;
  // hub.stats
  sessions?: number;
  uptime_s?: number;
  // hub.config.get / hub.config.set
  config?: HubConfig;
  options?: HubConfig;
  // hub.wifi.scan
  current?: WifiCurrent | null;
  networks?: WifiNetwork[];
  [key: string]: unknown;
}

export interface ErrMessage extends Envelope {
  t: "err";
  id?: number;
  code: LinkErrorCode;
  msg?: string;
  /** For a rejected hub.config.set: the offending key as a dotted path. */
  field?: string;
}

// ---------------------------------------------------------------------------
// Authentication, spec 7.3

export interface AuthTokenMessage extends Envelope {
  t: "auth.token";
  id: number;
  token: string;
}

export interface AuthPairMessage extends Envelope {
  t: "auth.pair";
  id: number;
  pin: string;
  name: string;
  role?: LinkRole;
}

export interface AuthListMessage extends Envelope {
  t: "auth.list";
  id: number;
}

export interface AuthClientInfo {
  cid: number;
  name: string;
  role: LinkRole;
  created?: string;
  last_seen?: string;
  online?: boolean;
}

export interface AuthRevokeMessage extends Envelope {
  t: "auth.revoke";
  id: number;
  cid: number;
}

export interface AuthSetRoleMessage extends Envelope {
  t: "auth.set_role";
  id: number;
  cid: number;
  role: LinkRole;
}

// ---------------------------------------------------------------------------
// Device inventory, spec 7.4

export interface DeviceInfo {
  handle: number;
  serial: string;
  name: string;
  platform?: number;
  fw?: string;
  /** Beta number, 0 for a final release. Absent means unknown, not 0 (spec 7.4, 1.1). */
  fw_beta?: number;
  outputs?: number;
  inputs?: number;
  wire_version?: number;
  state: DeviceState;
  link?: DeviceLink;
  locked_by?: number | null;
  [key: string]: unknown;
}

export interface DeviceListMessage extends Envelope {
  t: "device.list";
  id: number;
}

export interface DeviceAddedMessage extends Envelope {
  t: "device.added";
  device: DeviceInfo;
}

export interface DeviceChangedMessage extends Envelope {
  t: "device.changed";
  device: DeviceInfo;
}

export interface DeviceRemovedMessage extends Envelope {
  t: "device.removed";
  handle: number;
  serial?: string;
}

export interface DeviceRenameMessage extends Envelope {
  t: "device.rename";
  id: number;
  handle: number;
  name: string;
}

export interface DeviceSnapshotMessage extends Envelope {
  t: "device.snapshot";
  id: number;
  handle: number;
}

// ---------------------------------------------------------------------------
// Poll subscriptions, spec 7.6

export interface PollRequest {
  /** Session-chosen 0..15; comes back in every POLL frame. */
  slot: number;
  /** bRequest of the GET to poll. Must be classed read. */
  req: number;
  val: number;
  idx: number;
  len: number;
  hz: number;
}

export interface PollGrant {
  slot: number;
  hz: number;
}

export interface PollSubscribeMessage extends Envelope {
  t: "poll.subscribe";
  id: number;
  handle: number;
  polls: PollRequest[];
}

export interface PollUnsubscribeMessage extends Envelope {
  t: "poll.unsubscribe";
  id: number;
  handle: number;
  slots: number[];
}

export interface PollErrorMessage extends Envelope {
  t: "poll.error";
  handle: number;
  slot?: number;
  code?: LinkErrorCode;
  msg?: string;
}

// ---------------------------------------------------------------------------
// Locks, spec 7.7

export interface LockAcquireMessage extends Envelope {
  t: "lock.acquire";
  id: number;
  handle: number;
  reason?: string;
  timeout_ms?: number;
}

export interface LockReleaseMessage extends Envelope {
  t: "lock.release";
  id: number;
  handle: number;
}

// ---------------------------------------------------------------------------
// Firmware install, spec 7.8

export interface FwInstallMessage extends Envelope {
  t: "fw.install";
  id: number;
  handle: number;
  size: number;
  sha256: string;
  version?: string;
}

export type FwPhase = "uploading" | "rebooting" | "copying" | "verifying";

export interface FwProgressMessage extends Envelope {
  t: "fw.progress";
  handle: number;
  phase: FwPhase;
  pct?: number;
}

export interface FwDoneMessage extends Envelope {
  t: "fw.done";
  handle: number;
  ok: boolean;
  fw?: string;
  fw_beta?: number;
}

// ---------------------------------------------------------------------------
// Hub management, spec 7.9

export interface HubStatsMessage extends Envelope {
  t: "hub.stats";
  id: number;
}

export interface HubRenameMessage extends Envelope {
  t: "hub.rename";
  id: number;
  name: string;
}

// ---------------------------------------------------------------------------
// Hub settings, spec 7.10

/**
 * A hub's configuration. Its keys belong to the hub kind; this spec defines
 * only `name`. The bridge's keys are in esp32_bridge_spec.md.
 */
export interface HubConfig {
  name?: string;
  [key: string]: unknown;
}

export interface HubConfigGetMessage extends Envelope {
  t: "hub.config.get";
  id: number;
}

/** Only the keys being changed; nested objects merge. */
export interface HubConfigSetMessage extends Envelope {
  t: "hub.config.set";
  id: number;
  config: HubConfig;
}

/** Sent to every admin session after any change. */
export interface HubConfigChangedMessage extends Envelope {
  t: "hub.config.changed";
  config: HubConfig;
}

// ---------------------------------------------------------------------------
// Wi-Fi, spec 7.11

export interface WifiNetwork {
  ssid: string;
  rssi: number;
  secure: boolean;
}

export interface WifiCurrent {
  ssid: string;
  rssi: number;
  ip: string;
}

export interface HubWifiScanMessage extends Envelope {
  t: "hub.wifi.scan";
  id: number;
}

export interface HubWifiJoinMessage extends Envelope {
  t: "hub.wifi.join";
  id: number;
  ssid: string;
  /** Empty for an open network. */
  password: string;
}

// ---------------------------------------------------------------------------
// Authentication settings, spec 7.12

/** `mode: "pin"` with a 6-digit `pin` sets a fixed PIN; without one, window PINs. */
export interface HubAuthSetMessage extends Envelope {
  t: "hub.auth.set";
  id: number;
  mode: AuthMode;
  pin?: string;
}

// ---------------------------------------------------------------------------
// Restart and reset, spec 7.13

export type HubResetLevel = "network" | "factory";

export interface HubRestartMessage extends Envelope {
  t: "hub.restart";
  id: number;
}

export interface HubResetMessage extends Envelope {
  t: "hub.reset";
  id: number;
  level: HubResetLevel;
}

// ---------------------------------------------------------------------------
// Hub firmware update, spec 7.14

export interface HubUpdateMessage extends Envelope {
  t: "hub.update";
  id: number;
  size: number;
  /** Lowercase hex. */
  sha256: string;
  version?: string;
}

export interface HubUpdateDoneMessage extends Envelope {
  t: "hub.update.done";
  ok: boolean;
  version?: string;
  msg?: string;
}

/** Anything this version does not model, kept addressable rather than dropped. */
export interface UnknownMessage extends Envelope {
  t: string;
  [key: string]: unknown;
}

export type LinkMessage =
  | HelloClientMessage
  | HelloHubMessage
  | OkMessage
  | ErrMessage
  | AuthTokenMessage
  | AuthPairMessage
  | AuthListMessage
  | AuthRevokeMessage
  | AuthSetRoleMessage
  | DeviceListMessage
  | DeviceAddedMessage
  | DeviceChangedMessage
  | DeviceRemovedMessage
  | DeviceRenameMessage
  | DeviceSnapshotMessage
  | PollSubscribeMessage
  | PollUnsubscribeMessage
  | PollErrorMessage
  | LockAcquireMessage
  | LockReleaseMessage
  | FwInstallMessage
  | FwProgressMessage
  | FwDoneMessage
  | HubStatsMessage
  | HubRenameMessage
  | HubConfigGetMessage
  | HubConfigSetMessage
  | HubConfigChangedMessage
  | HubWifiScanMessage
  | HubWifiJoinMessage
  | HubAuthSetMessage
  | HubRestartMessage
  | HubResetMessage
  | HubUpdateMessage
  | HubUpdateDoneMessage
  | UnknownMessage;

/** Thrown by decode for a text frame that is not a control message at all. */
export class LinkMessageError extends Error {
  /** @param message What is wrong with the text frame. */
  constructor(message: string) {
    super(message);
    this.name = "LinkMessageError";
  }
}

/**
 * Serialises a control message to the text frame that goes on the socket.
 *
 * @param message The message, with its `id` already set for a request.
 * @returns The JSON text.
 */
export function encode(message: LinkMessage): string {
  return JSON.stringify(message);
}

/**
 * Parses a text frame. Unknown `t` values come back as UnknownMessage rather
 * than throwing; unknown fields on known types are carried through untouched,
 * so decode/encode is lossless and a newer hub's extra keys survive a relay.
 * Only malformed JSON, a non-object, or a missing/non-string `t` is an error.
 *
 * @param json The frame's text, or a value already parsed from JSON.
 * @returns The message.
 * @throws LinkMessageError when it is not a control message.
 */
export function decode(json: unknown): LinkMessage {
  let parsed: unknown = json;
  if (typeof json === "string") {
    try {
      parsed = JSON.parse(json);
    } catch (error) {
      throw new LinkMessageError(`not JSON: ${(error as Error).message}`);
    }
  }
  if (parsed === null || typeof parsed !== "object" || Array.isArray(parsed)) {
    throw new LinkMessageError("control message must be a JSON object");
  }
  const obj = parsed as Record<string, unknown>;
  if (typeof obj.t !== "string") {
    throw new LinkMessageError("control message needs a string `t`");
  }
  return { ...obj } as LinkMessage;
}

/**
 * @param message A decoded message.
 * @returns True for an `ok` reply.
 */
export function isOk(message: LinkMessage): message is OkMessage {
  return message.t === "ok";
}

/**
 * @param message A decoded message.
 * @returns True for an `err` reply.
 */
export function isErr(message: LinkMessage): message is ErrMessage {
  return message.t === "err";
}

/**
 * The messages a hub sends a client, by type. Because LinkMessage includes
 * UnknownMessage, whose `t` is any string, a `switch` on `t` cannot narrow;
 * isMessage does, for these.
 */
export interface HubMessageByType {
  hello: HelloHubMessage;
  ok: OkMessage;
  err: ErrMessage;
  "device.added": DeviceAddedMessage;
  "device.changed": DeviceChangedMessage;
  "device.removed": DeviceRemovedMessage;
  "poll.error": PollErrorMessage;
  "fw.progress": FwProgressMessage;
  "fw.done": FwDoneMessage;
  "hub.config.changed": HubConfigChangedMessage;
  "hub.update.done": HubUpdateDoneMessage;
}

/**
 * Narrows a decoded message by its type. It checks only `t`: the fields are
 * as the hub sent them, so optional ones may still be missing.
 *
 * @param message A decoded message.
 * @param type The `t` to test for.
 * @returns True when `message.t` is `type`.
 */
export function isMessage<K extends keyof HubMessageByType>(
  message: LinkMessage,
  type: K,
): message is HubMessageByType[K] {
  return message.t === type;
}
