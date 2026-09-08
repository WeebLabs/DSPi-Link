/*
 * Control-plane messages, DSPi Link protocol spec section 7.
 *
 * Every text frame is one JSON object with a string `t`. Requests carry an
 * integer `id`; replies echo it as `t: "ok"` or `t: "err"`; events have no id.
 * Wire keys are snake_case and are written here exactly as they appear on the
 * wire, so the types double as the wire contract.
 *
 * Unknown fields are preserved on decode and unknown `t` values decode to a
 * bare envelope, because spec 10.3 requires both sides to ignore rather than
 * reject what they do not recognise.
 */

export interface ProtoVersion {
  major: number;
  minor: number;
}

export type AuthMode = "none" | "pin";
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
  caps: LinkCapability[];
  limits: HubLimits;
}

export type HelloMessage = HelloClientMessage | HelloHubMessage;

/** A hub hello carries `hub`; a client hello carries `client`. */
export function isHubHello(msg: HelloMessage): msg is HelloHubMessage {
  return (msg as HelloHubMessage).hub !== undefined;
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
  // fw.install
  xfer?: number;
  // hub.stats
  sessions?: number;
  uptime_s?: number;
  [key: string]: unknown;
}

export interface ErrMessage extends Envelope {
  t: "err";
  id?: number;
  code: LinkErrorCode;
  msg?: string;
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
  | UnknownMessage;

export class LinkMessageError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "LinkMessageError";
  }
}

/** Serialise a control message to the text frame that goes on the socket. */
export function encode(msg: LinkMessage): string {
  return JSON.stringify(msg);
}

/**
 * Parse a text frame. Unknown `t` values come back as UnknownMessage rather
 * than throwing; unknown fields on known types are carried through untouched,
 * so decode/encode is lossless and a newer hub's extra keys survive a relay.
 * Only malformed JSON, a non-object, or a missing/non-string `t` is an error.
 */
export function decode(json: string | unknown): LinkMessage {
  let parsed: unknown = json;
  if (typeof json === "string") {
    try {
      parsed = JSON.parse(json);
    } catch (e) {
      throw new LinkMessageError(`not JSON: ${(e as Error).message}`);
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

export function isOk(msg: LinkMessage): msg is OkMessage {
  return msg.t === "ok";
}

export function isErr(msg: LinkMessage): msg is ErrMessage {
  return msg.t === "err";
}
