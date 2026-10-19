/*
 * LinkClient: one authenticated WebSocket session to a DSPi Link hub.
 *
 * Owns the handshake and authentication (Link spec 5 and 6), request and
 * reply correlation on both planes (7.1 and 8.2), the device inventory (7.4),
 * poll subscriptions (7.6), locks (7.7), the hub administration calls (7.9 to
 * 7.13), keepalive and reconnection. Firmware upload (7.14) is in
 * hubUpdate.ts. It does not interpret command payloads: those are the
 * device's bytes and the caller's business.
 *
 * Every JavaScript program that talks Link, the hub's own web page included,
 * goes through this class, so the session rules live in one place.
 */

import {
  LINK_LIBRARY_VERSION,
  LINK_PROTO_VERSION,
  LINK_SUBPROTOCOL,
} from "./constants.js";
import { base64ToBytes } from "./base64.js";
import { asError, LinkCommandError, LinkProtocolError } from "./errors.js";
import {
  type CmdRequestFrame,
  type CmdResponseFrame,
  type NotifyFrame,
  type PollFrame,
  type ResyncFrame,
  LinkStatus,
  ResyncReason,
  decodeFrame,
  encodeFrame,
} from "./frames.js";
import { HubUpdater, type HubUpdateOptions } from "./hubUpdate.js";
import {
  type AuthClientInfo,
  type AuthPairMessage,
  type AuthTokenMessage,
  type DeviceInfo,
  type ErrMessage,
  type HelloHubMessage,
  type HubAuthSetMessage,
  type HubConfig,
  type HubLimits,
  type HubResetLevel,
  type LinkCapability,
  type LinkMessage,
  type LinkRole,
  type LockAcquireMessage,
  type OkMessage,
  type PollGrant,
  type PollRequest,
  type PolicyDescriptor,
  type WifiCurrent,
  type WifiNetwork,
  decode as decodeMessage,
  encode as encodeMessage,
  isMessage,
} from "./messages.js";

export type { HubUpdateOptions } from "./hubUpdate.js";

/** Where a LinkClient is in its session's life. */
export type LinkClientState =
  | "idle"
  | "connecting"
  /** Socket open, hello exchanged or in flight. */
  | "handshaking"
  /** A token or PIN is with the hub, waiting for ok. */
  | "authenticating"
  /** The hub wants a PIN: no token stored, or the stored one was rejected. */
  | "awaiting_pairing"
  | "ready"
  | "reconnecting"
  | "closed";

/**
 * Why the socket closed, for the close codes of Link spec 5. 1001 (keepalive
 * failure), 1011 (client stopped reading) and any other code are "abnormal",
 * which the client retries.
 */
export type LinkCloseReason =
  | "normal"
  | "protocol_error"
  | "auth_failed"
  | "revoked"
  | "shutting_down"
  | "abnormal";

/**
 * Maps a WebSocket close code to its meaning.
 *
 * @param code The close event's code.
 * @returns The reason the code stands for (Link spec 5).
 */
export function closeReasonFor(code: number): LinkCloseReason {
  switch (code) {
    case 1000:
      return "normal";
    case 4000:
      return "protocol_error";
    case 4001:
      return "auth_failed";
    case 4002:
      return "revoked";
    case 4003:
      return "shutting_down";
    default:
      return "abnormal";
  }
}

/** Reasons a retry would end the same way: a deliberate close, a protocol
 * mismatch, a refused auth or a revoked token. */
const FINAL_CLOSE_REASONS: ReadonlySet<LinkCloseReason> = new Set([
  "normal",
  "protocol_error",
  "auth_failed",
  "revoked",
]);

/** The payload of the `close` event. */
export interface LinkCloseInfo {
  code: number;
  reason: LinkCloseReason;
  /** True when the client will try to reconnect. */
  willReconnect: boolean;
  message?: string;
}

/** Settings for a LinkClient; every one has a default. */
export interface LinkClientOptions {
  /** WebSocket implementation, for Node versions or tests without a global one. */
  webSocket?: WebSocketLike;
  /** Reconnect after an unexpected close. Off by default so tests stay deterministic. */
  reconnect?: boolean;
  /** Backoff cap, Link spec 5. Delays run 1, 2, 4, ... seconds up to this. */
  reconnectMaxMs?: number;
  /** How long a tunnelled command may take before it rejects. */
  commandTimeoutMs?: number;
  /** How long a control-plane request may take before it rejects. */
  requestTimeoutMs?: number;
  /** How long from opening the socket to `ready` or `awaiting_pairing`. */
  handshakeTimeoutMs?: number;
  /** Keepalive period. 0 disables it. */
  pingIntervalMs?: number;
}

/** What connect() tells the hub about this client. */
export interface ConnectOptions {
  /** Name the hub shows in its client list. */
  clientName: string;
  /**
   * Stored pairing token, if this client has one. It is tied to the first hub
   * that answers, and never offered to a hub with another id. Prefer
   * `tokenFor`, which picks the token by the id in the hub's hello.
   */
  token?: string;
  /**
   * Looks up the token stored for a hub id (Link spec 6.2), called once the
   * hub's hello has said which hub answered, on every connection, including
   * reconnects. Return null or undefined when there is none.
   */
  tokenFor?: (hubId: string) => string | null | undefined;
  /** The application's name, sent in hello. */
  app?: string;
  /** The application's version, sent in hello. */
  version?: string;
  /** Role to ask for when pairing. */
  role?: LinkRole;
}

/** The subset of the WebSocket constructor this client needs. */
export type WebSocketLike = new (url: string, protocols?: string | string[]) => WebSocketInstance;

/** The subset of a WebSocket this client needs. */
export interface WebSocketInstance {
  binaryType?: string;
  readyState: number;
  send(data: string | ArrayBufferLike | ArrayBufferView): void;
  close(code?: number, reason?: string): void;
  onopen: ((event: unknown) => void) | null;
  onclose: ((event: { code?: number; reason?: string }) => void) | null;
  onerror: ((event: unknown) => void) | null;
  onmessage: ((event: { data: unknown }) => void) | null;
  /** Present on Node's `ws`; absent in browsers, which cannot send control pings. */
  ping?: () => void;
}

/** The reply to device.snapshot (Link spec 7.5), with the blobs decoded. */
export interface SnapshotResult {
  handle: number;
  wire_version?: number;
  age_ms?: number;
  bulk_b64?: string;
  status_b64?: string;
  /** Decoded `bulk_b64`, the REQ_GET_ALL_PARAMS blob. */
  bulk?: Uint8Array;
  /** Decoded `status_b64`, the REQ_GET_STATUS wValue 9 response. */
  status?: Uint8Array;
}

/** The reply to hub.config.get (Link spec 7.10). */
export interface HubConfigResult {
  config: HubConfig;
  /** The permitted values of keys that come from a fixed set. */
  options: HubConfig;
}

/** The reply to hub.wifi.scan (Link spec 7.11). */
export interface WifiScanResult {
  current: WifiCurrent | null;
  networks: WifiNetwork[];
}

/** The events a LinkClient emits, with their payloads. */
export interface LinkClientEventMap {
  state: { state: LinkClientState; previous: LinkClientState };
  devices: DeviceInfo[];
  notify: NotifyFrame;
  poll: PollFrame;
  /**
   * Re-read the device's state (Link spec 8.6). Sent for the hub's RESYNC
   * frames, and also by the client itself, with reason 0, when it sees a gap
   * in the NOTIFY sequence or in the firmware's `seq` (Link spec 8.3).
   */
  resync: ResyncFrame;
  message: LinkMessage;
  /** The hub's configuration changed, by this session or another (Link spec 7.10). */
  hubConfig: HubConfig;
  /** The hub refused or revoked the token held for `hubId`; forget the stored copy. */
  tokenRejected: { hubId: string };
  error: Error;
  close: LinkCloseInfo;
}

type Listener<K extends keyof LinkClientEventMap> = (payload: LinkClientEventMap[K]) => void;

interface Pending<T> {
  resolve: (value: T) => void;
  reject: (error: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

const RECONNECT_BASE_MS = 1000;
const DEFAULT_RECONNECT_MAX_MS = 30000;
const DEFAULT_COMMAND_TIMEOUT_MS = 10000;
const DEFAULT_REQUEST_TIMEOUT_MS = 10000;
/** Enough for a slow LAN and a hub that reads its device list before answering. */
const DEFAULT_HANDSHAKE_TIMEOUT_MS = 15000;
/** Half the hub's 30 s idle limit (Link spec 5, close code 1001). */
const DEFAULT_PING_INTERVAL_MS = 15000;

/** A Wi-Fi scan takes a few seconds and the hub answers within 10 s, so
 * this leaves room for the round trip on top. */
const WIFI_SCAN_TIMEOUT_MS = 15000;

/** An open-access hub ignores the PIN of auth.pair (Link spec 6.1). */
const OPEN_ACCESS_PIN = "000000";

/** The FWDATA chunk bound to assume when a hub's hello gives no max_frame. */
const DEFAULT_MAX_FRAME_BYTES = 16384;

/** Request ids stay positive 31-bit integers, which every JSON parser holds exactly. */
const MAX_REQUEST_ID = 0x7fffffff;
/** CMD tags are u16, and 0 is never used. */
const MAX_COMMAND_TAG = 0xffff;

/** The close code a client sends when it gives up on a socket itself. */
const CLIENT_CLOSE_CODE = 1000;
/** The code this client reports for a socket it abandoned (no close event came). */
const ABANDONED_CLOSE_CODE = 1006;

/** Error codes that mean the hub refused the stored token itself. */
const TOKEN_REJECTION_CODES: ReadonlySet<string> = new Set(["unauthenticated", "denied"]);

/** NOTIFY tags are u16 (Link spec 8.3); the firmware's `seq` is a u8. */
const NOTIFY_TAG_MASK = 0xffff;
const FIRMWARE_SEQ_MASK = 0xff;
/** A v2 notification packet: version byte 2, and `seq` at byte 3. */
const NOTIFY_PACKET_VERSION = 2;
const NOTIFY_PACKET_SEQ_OFFSET = 3;

/** One session to a hub. See the file comment. */
export class LinkClient {
  private socket: WebSocketInstance | null = null;
  private readonly WebSocketConstructor: WebSocketLike;
  private readonly options: Required<Omit<LinkClientOptions, "webSocket">>;

  private url = "";
  private connectOptions: ConnectOptions = { clientName: "DSPi client" };

  private _state: LinkClientState = "idle";
  private _hub: HelloHubMessage | null = null;
  private _session: number | null = null;
  private _role: LinkRole | null = null;
  private _policy: PolicyDescriptor | null = null;
  private _token: string | null = null;
  /** The hub `_token` belongs to; null until a hello has said which hub answered. */
  private tokenHubId: string | null = null;
  private devices = new Map<number, DeviceInfo>();

  /** The last NOTIFY tag of this session, to spot a gap (Link spec 8.3). */
  private lastNotifyTag: number | null = null;
  /** Per device, the firmware `seq` of the last notification packet. */
  private readonly lastFirmwareSeq = new Map<number, number>();

  private nextId = 1;
  private nextTag = 1;
  private readonly pendingRequests = new Map<number, Pending<OkMessage>>();
  private readonly pendingCommands = new Map<number, Pending<CmdResponseFrame>>();

  /** Remembered so a reconnect, or a device coming back, restores the same meter feeds. */
  private readonly pollSubscriptions = new Map<number, Map<number, PollRequest>>();

  private readonly listeners = new Map<string, Set<(payload: never) => void>>();

  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private reconnectAttempt = 0;
  private manualClose = false;
  private handshake: Pending<void> | null = null;

  private readonly updater = new HubUpdater({
    request: (message, timeoutMs) => this.request(message, timeoutMs),
    onMessage: (listener) => this.on("message", listener),
    sendFrame: (frame) => this.requireSocket().send(encodeFrame(frame)),
    maxFrameBytes: () => this.limits?.max_frame ?? DEFAULT_MAX_FRAME_BYTES,
  });

  /**
   * @param options Timeouts, reconnection and the WebSocket implementation.
   * @throws Error when there is no global WebSocket and none is given.
   */
  constructor(options: LinkClientOptions = {}) {
    const implementation =
      options.webSocket ?? (globalThis as { WebSocket?: WebSocketLike }).WebSocket;
    if (!implementation) {
      throw new Error("no WebSocket available; pass options.webSocket");
    }
    this.WebSocketConstructor = implementation;
    this.options = {
      reconnect: options.reconnect ?? false,
      reconnectMaxMs: options.reconnectMaxMs ?? DEFAULT_RECONNECT_MAX_MS,
      commandTimeoutMs: options.commandTimeoutMs ?? DEFAULT_COMMAND_TIMEOUT_MS,
      requestTimeoutMs: options.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS,
      handshakeTimeoutMs: options.handshakeTimeoutMs ?? DEFAULT_HANDSHAKE_TIMEOUT_MS,
      pingIntervalMs: options.pingIntervalMs ?? DEFAULT_PING_INTERVAL_MS,
    };
  }

  // -- accessors ------------------------------------------------------------

  /** Where the session is now; the `state` event reports every change. */
  get state(): LinkClientState {
    return this._state;
  }

  /** The hub's hello, once it has arrived. */
  get hub(): HelloHubMessage | null {
    return this._hub;
  }

  /** The hub's id (`hub.id` in hello), the key to store its token under (Link spec 6.2). */
  get hubId(): string | null {
    return this._hub?.hub.id ?? null;
  }

  /** Session id the hub stamps into NOTIFY frames as `origin`. */
  get session(): number | null {
    return this._session;
  }

  /** The role the hub granted, once authenticated. */
  get role(): LinkRole | null {
    return this._role;
  }

  /** The commands this session may not send, from the auth ok (Link spec 6.3). */
  get policy(): PolicyDescriptor | null {
    return this._policy;
  }

  /** The token from the last successful pairing, if the hub issued one. */
  get token(): string | null {
    return this._token;
  }

  /** The capabilities in the hub's hello; empty before it arrives. */
  get caps(): LinkCapability[] {
    return this._hub?.caps ?? [];
  }

  /** The limits in the hub's hello, once it has arrived. */
  get limits(): HubLimits | null {
    return this._hub?.limits ?? null;
  }

  /**
   * @param name A capability name (Link spec 7.2).
   * @returns True when the hub's hello lists it.
   */
  hasCap(name: LinkCapability): boolean {
    return this.caps.includes(name);
  }

  /** Every device the hub shares, in handle order. */
  get deviceList(): DeviceInfo[] {
    return [...this.devices.values()].sort((a, b) => a.handle - b.handle);
  }

  /**
   * @param handle A device handle.
   * @returns The device, or undefined when the hub has none with that handle.
   */
  device(handle: number): DeviceInfo | undefined {
    return this.devices.get(handle);
  }

  // -- events ---------------------------------------------------------------

  /**
   * Listens for an event.
   *
   * @param event The event's name.
   * @param listener Called with the event's payload.
   * @returns A function that removes the listener; call it when done.
   */
  on<K extends keyof LinkClientEventMap>(event: K, listener: Listener<K>): () => void {
    let set = this.listeners.get(event);
    if (!set) {
      set = new Set();
      this.listeners.set(event, set);
    }
    set.add(listener as (payload: never) => void);
    return () => this.off(event, listener);
  }

  /**
   * Removes a listener added with on().
   *
   * @param event The event's name.
   * @param listener The listener to remove.
   */
  off<K extends keyof LinkClientEventMap>(event: K, listener: Listener<K>): void {
    this.listeners.get(event)?.delete(listener as (payload: never) => void);
  }

  private emit<K extends keyof LinkClientEventMap>(event: K, payload: LinkClientEventMap[K]): void {
    const set = this.listeners.get(event);
    if (!set) return;
    for (const listener of [...set]) {
      try {
        (listener as Listener<K>)(payload);
      } catch (error) {
        // A listener throwing must not tear down the socket loop.
        if (event !== "error") this.emit("error", asError(error));
      }
    }
  }

  private setState(next: LinkClientState): void {
    if (this._state === next) return;
    const previous = this._state;
    this._state = next;
    this.emit("state", { state: next, previous });
  }

  // -- connection -----------------------------------------------------------

  /**
   * Opens a session, replacing any session this client already has.
   *
   * @param url The hub's WebSocket URL, as webSocketUrl() builds it.
   * @param options The name, token and role to present.
   * @returns Resolves once the handshake settles: `ready` when the hub
   *   accepted us, `awaiting_pairing` when it wants a PIN (call pair()).
   *   Rejects when the socket closes or the hub does not answer in time.
   */
  connect(url: string, options: ConnectOptions): Promise<void> {
    this.releaseSocket(new Error("replaced by a new connection"));
    this.url = url;
    this.connectOptions = options;
    this._token = options.token ?? null;
    this.tokenHubId = null;
    this.manualClose = false;
    this.reconnectAttempt = 0;
    return this.open();
  }

  private open(): Promise<void> {
    this.cancelReconnect();
    this.setState("connecting");
    return new Promise<void>((resolve, reject) => {
      let socket: WebSocketInstance;
      try {
        socket = new this.WebSocketConstructor(this.url, LINK_SUBPROTOCOL);
      } catch (error) {
        this.setState("closed");
        reject(asError(error));
        return;
      }
      const timer = setTimeout(
        () => this.onHandshakeTimeout(socket),
        this.options.handshakeTimeoutMs,
      );
      this.handshake = { resolve, reject, timer };
      this.attach(socket);
    });
  }

  /**
   * Makes `socket` the current one. Every handler checks that it still is,
   * so a socket that has been replaced or abandoned can no longer touch the
   * session, even if its close event arrives late.
   */
  private attach(socket: WebSocketInstance): void {
    this.socket = socket;
    try {
      socket.binaryType = "arraybuffer";
    } catch {
      // Some implementations only allow this after open; harmless either way.
    }
    socket.onopen = () => {
      if (socket === this.socket) this.onOpen();
    };
    socket.onmessage = (event) => {
      if (socket === this.socket) this.onMessage(event.data);
    };
    socket.onerror = () => {
      if (socket === this.socket) this.emit("error", new Error("websocket error"));
    };
    socket.onclose = (event) => {
      if (socket === this.socket) this.onClose(event?.code ?? ABANDONED_CLOSE_CODE, event?.reason);
    };
  }

  /** Stops listening to `socket` and closes it. */
  private detach(socket: WebSocketInstance, code: number, reason: string): void {
    socket.onopen = null;
    socket.onmessage = null;
    socket.onerror = null;
    socket.onclose = null;
    try {
      socket.close(code, reason);
    } catch {
      // Already closing or closed; nothing is listening to it any more anyway.
    }
  }

  /** Drops the current socket, if any, failing everything that waited on it. */
  private releaseSocket(error: Error, code = CLIENT_CLOSE_CODE, reason = "client closing"): void {
    this.cancelReconnect();
    this.stopPing();
    const socket = this.socket;
    this.socket = null;
    if (socket) this.detach(socket, code, reason);
    this.failPending(error);
    this.rejectHandshake(error);
  }

  /**
   * Closes the session and stops reconnecting. Emits `close` at once, since
   * the socket's own close event is no longer listened to.
   *
   * @param code The close code to send (1000, or 3000 to 4999).
   * @param reason The close reason to send.
   */
  disconnect(code = CLIENT_CLOSE_CODE, reason = "client closing"): void {
    this.manualClose = true;
    const wasOpen = this.socket !== null;
    this.releaseSocket(new Error("disconnected"), code, reason);
    this.setState("closed");
    if (wasOpen) {
      this.emit("close", { code, reason: closeReasonFor(code), willReconnect: false, message: reason });
    }
  }

  private onOpen(): void {
    // A new session numbers its NOTIFY frames afresh, and anything missed
    // while disconnected is re-read on reconnecting anyway.
    this.lastNotifyTag = null;
    this.lastFirmwareSeq.clear();
    this.setState("handshaking");
    this.send({
      t: "hello",
      proto: { ...LINK_PROTO_VERSION },
      client: {
        name: this.connectOptions.clientName,
        app: this.connectOptions.app ?? "dspi-link-js",
        version: this.connectOptions.version ?? LINK_LIBRARY_VERSION,
      },
    });
  }

  /** The hub said nothing useful in time: give up on this socket as if it had dropped. */
  private onHandshakeTimeout(socket: WebSocketInstance): void {
    if (socket !== this.socket) return;
    this.rejectHandshake(new Error("the hub did not answer in time"));
    this.abandonSocket(socket, "handshake timed out");
  }

  /**
   * Gives up on a socket that is open but useless (no answer to the
   * handshake or to keepalives). A browser may not report such a socket
   * closed for minutes, so the client treats it as an abnormal close now,
   * which reconnects when reconnection is on.
   */
  private abandonSocket(socket: WebSocketInstance, reason: string): void {
    this.detach(socket, CLIENT_CLOSE_CODE, reason);
    this.onClose(ABANDONED_CLOSE_CODE, reason);
  }

  private onClose(code: number, message?: string): void {
    this.stopPing();
    this.socket = null;
    const reason = closeReasonFor(code);
    const willReconnect =
      this.options.reconnect && !this.manualClose && !FINAL_CLOSE_REASONS.has(reason);

    this.failPending(new Error(`connection closed (${code})`));
    if (reason === "revoked") this.dropToken();
    this.emit("close", { code, reason, willReconnect, message });
    this.rejectHandshake(new Error(`connection closed during handshake (${code})`));

    if (willReconnect) {
      this.setState("reconnecting");
      this.scheduleReconnect();
    } else {
      this.setState("closed");
    }
  }

  private scheduleReconnect(): void {
    const delayMs = Math.min(
      RECONNECT_BASE_MS * 2 ** this.reconnectAttempt,
      this.options.reconnectMaxMs,
    );
    this.reconnectAttempt++;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.open().catch((error: unknown) => this.emit("error", asError(error)));
    }, delayMs);
  }

  private cancelReconnect(): void {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
  }

  // -- inbound --------------------------------------------------------------

  private onMessage(data: unknown): void {
    if (typeof data === "string") {
      this.onText(data);
      return;
    }
    const bytes = toBytes(data);
    if (bytes) this.onBinary(bytes);
  }

  private onText(text: string): void {
    let message: LinkMessage;
    try {
      message = decodeMessage(text);
    } catch (error) {
      this.emit("error", asError(error));
      return;
    }
    this.emit("message", message);

    if (isMessage(message, "hello")) {
      this.onHubHello(message);
    } else if (isMessage(message, "ok") || isMessage(message, "err")) {
      this.settleRequest(message);
    } else if (isMessage(message, "device.added") || isMessage(message, "device.changed")) {
      if (message.device) this.onDeviceUpdate(message.device);
    } else if (isMessage(message, "device.removed")) {
      this.onDeviceRemoved(message.handle);
    } else if (isMessage(message, "hub.config.changed")) {
      if (message.config) this.emit("hubConfig", message.config);
    }
    // Anything else is an event this client does not act on (Link spec 10,
    // rule 3); listeners of `message` have already seen it.
  }

  private onDeviceUpdate(device: DeviceInfo): void {
    const previous = this.devices.get(device.handle);
    this.devices.set(device.handle, device);
    // A device that is new or back has restarted its notification `seq`.
    if (!previous || (previous.state !== "online" && device.state === "online")) {
      this.lastFirmwareSeq.delete(device.handle);
    }
    this.emit("devices", this.deviceList);
    // A device going offline ends its poll subscriptions at the hub (Link
    // spec 7.6), so they are sent again when it comes back.
    if (previous && previous.state !== "online" && device.state === "online") {
      this.restorePolls(device.handle).catch((error: unknown) =>
        this.emit("error", asError(error)),
      );
    }
  }

  private onDeviceRemoved(handle: number | undefined): void {
    if (handle === undefined || !this.devices.delete(handle)) return;
    this.pollSubscriptions.delete(handle);
    this.lastFirmwareSeq.delete(handle);
    this.emit("devices", this.deviceList);
  }

  private onBinary(bytes: Uint8Array): void {
    let frame;
    try {
      frame = decodeFrame(bytes);
    } catch (error) {
      this.emit("error", asError(error));
      return;
    }
    if (!frame) return; // Unknown frame type: drop it (Link spec 10, rule 4).

    switch (frame.type) {
      case "cmd_response": {
        const pending = this.pendingCommands.get(frame.tag);
        if (!pending) return;
        this.pendingCommands.delete(frame.tag);
        clearTimeout(pending.timer);
        pending.resolve(frame);
        return;
      }
      case "notify":
        this.checkNotifySequence(frame);
        this.emit("notify", frame);
        return;
      case "poll":
        this.emit("poll", frame);
        return;
      case "resync":
        this.emit("resync", frame);
        return;
      case "fwdata_ack":
        this.updater.settleAck(frame);
        return;
      default:
        return;
    }
  }

  /**
   * A missed notification shows as a gap in the session's NOTIFY tags or in a
   * device's firmware `seq`; either way the client must re-read (Link spec
   * 8.3 and 12). The tag counts every device's notifications, so a gap there
   * could have been about any of them.
   */
  private checkNotifySequence(frame: NotifyFrame): void {
    const stale = new Set<number>();
    const expectedTag =
      this.lastNotifyTag === null ? frame.tag : (this.lastNotifyTag + 1) & NOTIFY_TAG_MASK;
    if (frame.tag !== expectedTag) {
      for (const handle of this.devices.keys()) stale.add(handle);
      stale.add(frame.handle);
    }
    this.lastNotifyTag = frame.tag;

    const packet = frame.packet;
    if (packet.length > NOTIFY_PACKET_SEQ_OFFSET && packet[0] === NOTIFY_PACKET_VERSION) {
      const seq = packet[NOTIFY_PACKET_SEQ_OFFSET];
      const last = this.lastFirmwareSeq.get(frame.handle);
      if (last !== undefined && seq !== ((last + 1) & FIRMWARE_SEQ_MASK)) stale.add(frame.handle);
      this.lastFirmwareSeq.set(frame.handle, seq);
    }

    for (const handle of stale) {
      this.emit("resync", { type: "resync", tag: 0, handle, reason: ResyncReason.NOTIFY_DROPPED });
    }
  }

  private onHubHello(hello: HelloHubMessage): void {
    this._hub = hello;
    const socket = this.socket;
    if (hello.auth === "none") {
      // Open access opens the session on hello, but the client still needs its
      // session id to suppress its own notification echoes, and auth.pair is
      // the documented way to ask for it (Link spec 6.1).
      this.setState("authenticating");
      const { clientName, role } = this.connectOptions;
      this.sendPair(OPEN_ACCESS_PIN, clientName, role).catch((error: unknown) =>
        this.onOpenAccessPairFailed(asError(error), socket),
      );
      return;
    }
    this.chooseToken(hello.hub?.id ?? null);
    if (this._token) {
      this.setState("authenticating");
      this.sendToken(this._token).catch((error: unknown) =>
        this.onAuthFailed(asError(error), socket),
      );
      return;
    }
    this.setState("awaiting_pairing");
    this.settleHandshake();
  }

  private async sendToken(token: string): Promise<void> {
    const message: AuthTokenMessage = { t: "auth.token", id: 0, token };
    const reply = await this.request(message);
    await this.onAuthenticated(reply);
  }

  /**
   * The automatic half of the handshake could not authenticate on `socket`.
   * If that socket has gone, onClose owns the state and the token is kept for
   * the next connection: losing the network is not the token's fault. Only a
   * refusal by the hub drops the token; then, or after a timeout, the user
   * can pair again.
   */
  private onAuthFailed(error: Error, socket: WebSocketInstance | null): void {
    if (socket !== this.socket) return;
    this.emit("error", error);
    if (error instanceof LinkProtocolError && TOKEN_REJECTION_CODES.has(error.code)) {
      this.dropToken();
    }
    this.setState("awaiting_pairing");
    this.settleHandshake();
  }

  /**
   * An open-access hub opened the session with its hello (Link spec 6.1);
   * the auth.pair was only to learn the session id. If it fails, the session
   * goes on without one, as an admin, rather than asking for a PIN the hub
   * does not want. Echo suppression is then off for this session.
   */
  private onOpenAccessPairFailed(error: Error, socket: WebSocketInstance | null): void {
    if (socket !== this.socket) return;
    this.emit("error", error);
    void this.onAuthenticated({ t: "ok", id: 0, role: "admin" });
  }

  /**
   * Picks the token to present to the hub whose hello just arrived. A token
   * held for another hub is never offered: a different hub now answers at
   * this address. With `tokenFor`, the token is looked up by the hub's id.
   */
  private chooseToken(hubId: string | null): void {
    if (this._token && this.tokenHubId !== null && this.tokenHubId !== hubId) {
      this._token = null;
      this.tokenHubId = null;
    }
    if (!this._token && hubId !== null && this.connectOptions.tokenFor) {
      this._token = this.connectOptions.tokenFor(hubId) ?? null;
    }
    if (this._token) this.tokenHubId = hubId;
  }

  /** Forgets a token the hub refused or revoked, and says so. */
  private dropToken(): void {
    const hubId = this.tokenHubId ?? this.hubId;
    const had = this._token !== null;
    this._token = null;
    this.tokenHubId = null;
    if (had && hubId) this.emit("tokenRejected", { hubId });
  }

  /** Keeps a token the hub just issued, tied to the hub that issued it. */
  private keepToken(token: string | null): void {
    this._token = token;
    this.tokenHubId = token === null ? null : this.hubId;
  }

  private async sendPair(pin: string, name: string, role?: LinkRole): Promise<string | null> {
    const message: AuthPairMessage = { t: "auth.pair", id: 0, pin, name };
    if (role) message.role = role;
    const reply = await this.request(message);
    await this.onAuthenticated(reply);
    return this._token;
  }

  private async onAuthenticated(reply: OkMessage): Promise<void> {
    this._session = reply.session ?? null;
    this._role = reply.role ?? null;
    this._policy = reply.policy ?? null;
    if (typeof reply.token === "string") this.keepToken(reply.token);
    this.setState("ready");
    this.reconnectAttempt = 0;
    this.startPing();
    try {
      await this.refreshDevices();
      await this.restorePollSubscriptions();
    } catch (error) {
      this.emit("error", asError(error));
    }
    this.settleHandshake();
  }

  private settleHandshake(): void {
    const handshake = this.handshake;
    if (!handshake) return;
    this.handshake = null;
    clearTimeout(handshake.timer);
    handshake.resolve();
  }

  private rejectHandshake(error: Error): void {
    const handshake = this.handshake;
    if (!handshake) return;
    this.handshake = null;
    clearTimeout(handshake.timer);
    handshake.reject(error);
  }

  // -- public API -----------------------------------------------------------

  /**
   * Pairs with a PIN the hub is showing, or the hub's fixed PIN.
   *
   * @param pin The 6-digit PIN.
   * @param clientName The name to list this client under; defaults to connect()'s.
   * @param role The role to ask for; the hub may grant another.
   * @returns The long-lived token to store against the hub id, or null when
   *   the hub runs open access and issues none. Rejects with
   *   LinkProtocolError for a wrong PIN, leaving the state awaiting_pairing.
   */
  pair(pin: string, clientName?: string, role?: LinkRole): Promise<string | null> {
    const name = clientName ?? this.connectOptions.clientName;
    if (clientName) this.connectOptions.clientName = clientName;
    if (role) this.connectOptions.role = role;
    this.setState("authenticating");
    return this.sendPair(pin, name, role ?? this.connectOptions.role).catch((error: unknown) => {
      this.setState("awaiting_pairing");
      throw error;
    });
  }

  /**
   * Re-reads the inventory and replaces the cached device map.
   *
   * @returns The devices, in handle order.
   */
  async refreshDevices(): Promise<DeviceInfo[]> {
    const reply = await this.request({ t: "device.list", id: 0 });
    this.devices = new Map((reply.devices ?? []).map((device) => [device.handle, device]));
    this.emit("devices", this.deviceList);
    return this.deviceList;
  }

  /**
   * Tunnels one vendor command and waits for its response frame (Link spec 8.2).
   *
   * @param request The command; the tag is chosen here.
   * @returns The response, whatever its status. Rejects on a timeout or a
   *   closed session.
   */
  command(request: Omit<CmdRequestFrame, "type" | "tag">): Promise<CmdResponseFrame> {
    const socket = this.requireSocket();
    const tag = this.allocateTag();
    const frame: CmdRequestFrame = { ...request, type: "cmd_request", tag };
    return new Promise<CmdResponseFrame>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pendingCommands.delete(tag);
        reject(new Error(`command 0x${request.bRequest.toString(16)} timed out`));
      }, this.options.commandTimeoutMs);
      this.pendingCommands.set(tag, { resolve, reject, timer });
      try {
        socket.send(encodeFrame(frame));
      } catch (error) {
        this.pendingCommands.delete(tag);
        clearTimeout(timer);
        reject(asError(error));
      }
    });
  }

  /**
   * command(), rejecting when the hub or device did not answer OK.
   *
   * @param request The command.
   * @returns The response payload.
   * @throws LinkCommandError for a status other than OK.
   */
  async commandOk(request: Omit<CmdRequestFrame, "type" | "tag">): Promise<Uint8Array> {
    const response = await this.command(request);
    if (response.status !== LinkStatus.OK) throw new LinkCommandError(response);
    return response.payload;
  }

  /**
   * A GET command.
   *
   * @param handle The device.
   * @param bRequest The vendor command id.
   * @param wLength The number of bytes requested.
   * @param wValue The command's wValue.
   * @param wIndex 2 for application commands.
   * @returns The response frame.
   */
  get(
    handle: number,
    bRequest: number,
    wLength: number,
    wValue = 0,
    wIndex = 2,
  ): Promise<CmdResponseFrame> {
    return this.command({ handle, direction: "get", bRequest, wValue, wIndex, wLength });
  }

  /**
   * A SET command.
   *
   * @param handle The device.
   * @param bRequest The vendor command id.
   * @param payload The bytes to send; wLength is their length.
   * @param wValue The command's wValue.
   * @param wIndex 2 for application commands.
   * @returns The response frame.
   */
  set(
    handle: number,
    bRequest: number,
    payload: Uint8Array,
    wValue = 0,
    wIndex = 2,
  ): Promise<CmdResponseFrame> {
    return this.command({ handle, direction: "set", bRequest, wValue, wIndex, payload });
  }

  /**
   * The hub's cached bulk blob and status frame, one round trip (Link spec 7.5).
   *
   * @param handle The device.
   * @returns The reply, with `bulk` and `status` decoded from base64.
   */
  async snapshot(handle: number): Promise<SnapshotResult> {
    const reply = await this.request({ t: "device.snapshot", id: 0, handle });
    const result: SnapshotResult = {
      handle: reply.handle ?? handle,
      wire_version: reply.wire_version,
      age_ms: reply.age_ms,
      bulk_b64: reply.bulk_b64,
      status_b64: reply.status_b64,
    };
    if (reply.bulk_b64) result.bulk = base64ToBytes(reply.bulk_b64);
    if (reply.status_b64) result.status = base64ToBytes(reply.status_b64);
    return result;
  }

  /**
   * Subscribes to periodic GETs the hub pushes back as POLL frames (Link spec
   * 7.6). The subscription is remembered from the moment it is asked for, so
   * an unsubscribe sent before the reply arrives still wins, and it is sent
   * again after a reconnect or when the device comes back online.
   *
   * @param handle The device.
   * @param polls The polls, each in its own slot.
   * @returns The rates the hub granted.
   */
  async subscribePolls(handle: number, polls: PollRequest[]): Promise<PollGrant[]> {
    let slots = this.pollSubscriptions.get(handle);
    if (!slots) {
      slots = new Map();
      this.pollSubscriptions.set(handle, slots);
    }
    for (const poll of polls) slots.set(poll.slot, poll);
    try {
      const reply = await this.request({ t: "poll.subscribe", id: 0, handle, polls });
      return reply.granted ?? [];
    } catch (error) {
      for (const poll of polls) {
        if (slots.get(poll.slot) === poll) slots.delete(poll.slot);
      }
      throw error;
    }
  }

  /**
   * Ends poll subscriptions. They are forgotten here first, so they are never
   * restored even if the request fails.
   *
   * @param handle The device.
   * @param slots The slots to end.
   */
  async unsubscribePolls(handle: number, slots: number[]): Promise<void> {
    const known = this.pollSubscriptions.get(handle);
    if (known) for (const slot of slots) known.delete(slot);
    await this.request({ t: "poll.unsubscribe", id: 0, handle, slots });
  }

  /**
   * Sends every remembered subscription again on a new session, from the
   * fresh device list. A handle the hub no longer has is forgotten (its
   * device.removed went by while disconnected); an offline device is left
   * until it comes back online. Each handle is restored on its own, so one
   * failure never costs another device its feed.
   */
  private async restorePollSubscriptions(): Promise<void> {
    for (const handle of [...this.pollSubscriptions.keys()]) {
      const device = this.devices.get(handle);
      if (!device) {
        this.pollSubscriptions.delete(handle);
        continue;
      }
      if (device.state !== "online") continue;
      try {
        await this.restorePolls(handle);
      } catch (error) {
        this.emit("error", asError(error));
      }
    }
  }

  private async restorePolls(handle: number): Promise<void> {
    const slots = this.pollSubscriptions.get(handle);
    if (!slots || slots.size === 0) return;
    await this.request({ t: "poll.subscribe", id: 0, handle, polls: [...slots.values()] });
  }

  /**
   * Takes a device's exclusive lock (Link spec 7.7).
   *
   * @param handle The device.
   * @param reason Shown to other clients.
   * @param timeoutMs When the hub releases it by itself; the hub caps it at 60 s.
   */
  async acquireLock(handle: number, reason?: string, timeoutMs?: number): Promise<void> {
    const message: LockAcquireMessage = { t: "lock.acquire", id: 0, handle };
    if (reason) message.reason = reason;
    if (timeoutMs !== undefined) message.timeout_ms = timeoutMs;
    await this.request(message);
  }

  /**
   * Releases a lock this session holds.
   *
   * @param handle The device.
   */
  async releaseLock(handle: number): Promise<void> {
    await this.request({ t: "lock.release", id: 0, handle });
  }

  /**
   * Changes a device's hub-stored name (Link spec 7.4; capability "rename").
   *
   * @param handle The device.
   * @param name The new name.
   */
  async renameDevice(handle: number, name: string): Promise<void> {
    await this.request({ t: "device.rename", id: 0, handle, name });
  }

  // -- hub administration (admin role; Link spec 7.9 to 7.13) ---------------
  //
  // Each group needs its capability in the hub's hello (hasCap): "rename",
  // "hub_config", "wifi", "auth_config" and "reset".

  /**
   * Renames the hub itself (Link spec 7.9).
   *
   * @param name The new name.
   */
  async renameHub(name: string): Promise<void> {
    await this.request({ t: "hub.rename", id: 0, name });
  }

  /**
   * The hub's counters (Link spec 7.9). A bridge adds a `bridge` object.
   *
   * @returns The whole ok reply.
   */
  hubStats(): Promise<OkMessage> {
    return this.request({ t: "hub.stats", id: 0 });
  }

  /**
   * Reads the hub's settings (Link spec 7.10).
   *
   * @returns The configuration and the permitted values of its fixed-set keys.
   */
  async getHubConfig(): Promise<HubConfigResult> {
    const reply = await this.request({ t: "hub.config.get", id: 0 });
    return { config: reply.config ?? {}, options: reply.options ?? {} };
  }

  /**
   * Changes only the keys in `change`; nested objects merge (Link spec 7.10).
   *
   * @param change The keys to change.
   * @returns The whole configuration after the change. A rejected value
   *   rejects with a LinkProtocolError whose `field` names the key.
   */
  async setHubConfig(change: HubConfig): Promise<HubConfig> {
    const reply = await this.request({ t: "hub.config.set", id: 0, config: change });
    return reply.config ?? {};
  }

  /**
   * Scans for Wi-Fi networks (Link spec 7.11).
   *
   * @returns Nearby networks, strongest first, and the one the hub is on.
   */
  async scanWifi(): Promise<WifiScanResult> {
    const reply = await this.request({ t: "hub.wifi.scan", id: 0 }, WIFI_SCAN_TIMEOUT_MS);
    return { current: reply.current ?? null, networks: reply.networks ?? [] };
  }

  /**
   * Asks the hub to move to another network (Link spec 7.11). Resolving
   * means the hub took the request, not that the join worked: the hub then
   * closes every session (code 4003) and either joins or goes back to its
   * old network.
   *
   * @param ssid The network to join.
   * @param password Its password; empty for an open network.
   */
  async joinWifi(ssid: string, password: string): Promise<void> {
    await this.request({ t: "hub.wifi.join", id: 0, ssid, password });
  }

  /**
   * Turns on a fixed PIN (a 6-digit `pin`), window PINs (`pin` left out), or
   * open access (`mode` "none") (Link spec 7.12). Every token is revoked;
   * this session stays open and, in pin mode, gets a fresh token, which is
   * kept here for reconnecting.
   *
   * @param mode "pin" or "none".
   * @param pin The fixed PIN, for mode "pin".
   * @returns The new token, to store as connect() would, or null.
   */
  async setAuth(mode: "none" | "pin", pin?: string): Promise<string | null> {
    const message: HubAuthSetMessage = { t: "hub.auth.set", id: 0, mode };
    if (pin !== undefined) message.pin = pin;
    const reply = await this.request(message);
    this.keepToken(typeof reply.token === "string" ? reply.token : null);
    // The hello this session began with no longer says how the hub
    // authenticates; keep it truthful for whoever reads `hub` next.
    if (this._hub) {
      const hello: HelloHubMessage = { ...this._hub, auth: mode };
      if (mode === "pin") hello.pin_source = pin === undefined ? "window" : "fixed";
      else delete hello.pin_source;
      this._hub = hello;
    }
    return this._token;
  }

  /**
   * The paired clients, for a hub in pin mode (Link spec 6.2 and 7.3).
   *
   * @returns One record per paired client.
   */
  async listClients(): Promise<AuthClientInfo[]> {
    const reply = await this.request({ t: "auth.list", id: 0 });
    return reply.clients ?? [];
  }

  /**
   * Revokes one client's token; its live sessions close with code 4002.
   *
   * @param clientId The client's `cid` from listClients().
   */
  async revokeClient(clientId: number): Promise<void> {
    await this.request({ t: "auth.revoke", id: 0, cid: clientId });
  }

  /** Restarts the hub (Link spec 7.13): it replies ok, then closes every session (4003). */
  async restartHub(): Promise<void> {
    await this.request({ t: "hub.restart", id: 0 });
  }

  /**
   * Erases the network connection only, or all hub state, then restarts
   * (Link spec 7.13).
   *
   * @param level "network" or "factory".
   */
  async resetHub(level: HubResetLevel): Promise<void> {
    await this.request({ t: "hub.reset", id: 0, level });
  }

  /**
   * Installs new firmware on the hub itself (Link spec 7.14; capability
   * "hub_update"). The hub then closes every session (4003) and restarts
   * into it; the `hello` after reconnecting shows which firmware is running.
   *
   * @param image The whole firmware file.
   * @param options The version to log and a progress callback.
   * @returns The new version, once the hub has checked the image. Rejects
   *   with the hub's reason if it refuses the request or the image.
   */
  updateHub(image: Uint8Array, options: HubUpdateOptions = {}): Promise<string> {
    return this.updater.run(image, options);
  }

  // -- plumbing -------------------------------------------------------------

  /**
   * Sends a request and waits for its ok or err. The `id` is assigned here.
   *
   * @param message The request; any `id` in it is replaced.
   * @param timeoutMs How long to wait for the reply.
   * @returns The ok reply. Rejects with LinkProtocolError for an err reply,
   *   or Error on a timeout or a closed session.
   */
  request<T extends LinkMessage = LinkMessage>(
    message: T | Record<string, unknown>,
    timeoutMs = this.options.requestTimeoutMs,
  ): Promise<OkMessage> {
    const socket = this.requireSocket();
    const id = this.nextId++;
    if (this.nextId > MAX_REQUEST_ID) this.nextId = 1;
    const framed = { ...(message as Record<string, unknown>), id } as LinkMessage;
    return new Promise<OkMessage>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pendingRequests.delete(id);
        reject(new Error(`request ${framed.t} timed out`));
      }, timeoutMs);
      this.pendingRequests.set(id, { resolve, reject, timer });
      try {
        socket.send(encodeMessage(framed));
      } catch (error) {
        this.pendingRequests.delete(id);
        clearTimeout(timer);
        reject(asError(error));
      }
    });
  }

  private settleRequest(reply: OkMessage | ErrMessage): void {
    if (reply.id === undefined) return;
    const pending = this.pendingRequests.get(reply.id);
    if (!pending) return;
    this.pendingRequests.delete(reply.id);
    clearTimeout(pending.timer);
    if (reply.t === "ok") pending.resolve(reply);
    else pending.reject(new LinkProtocolError(reply.code, reply.msg, reply.field));
  }

  private send(message: LinkMessage): void {
    this.requireSocket().send(encodeMessage(message));
  }

  private requireSocket(): WebSocketInstance {
    if (!this.socket) throw new Error("not connected");
    return this.socket;
  }

  /** u16 request ids, skipping 0 and anything still outstanding. */
  private allocateTag(): number {
    for (let attempt = 0; attempt < MAX_COMMAND_TAG; attempt++) {
      const tag = this.nextTag;
      this.nextTag = this.nextTag >= MAX_COMMAND_TAG ? 1 : this.nextTag + 1;
      if (!this.pendingCommands.has(tag)) return tag;
    }
    throw new Error("no free command tag");
  }

  private failPending(error: Error): void {
    for (const pending of this.pendingRequests.values()) {
      clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.pendingRequests.clear();
    for (const pending of this.pendingCommands.values()) {
      clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.pendingCommands.clear();
    this.updater.failAll(error);
  }

  private startPing(): void {
    this.stopPing();
    if (this.options.pingIntervalMs <= 0) return;
    this.pingTimer = setInterval(() => this.ping(), this.options.pingIntervalMs);
  }

  private stopPing(): void {
    if (this.pingTimer) {
      clearInterval(this.pingTimer);
      this.pingTimer = null;
    }
  }

  /**
   * Keepalive. Browsers cannot send a WebSocket control ping, so where the
   * implementation offers one (Node's `ws`) it is used, and otherwise a cheap
   * request goes instead. The hub answers `unknown_type`, which is a perfectly
   * good round trip. No answer at all means the socket is half open, and it
   * is abandoned so reconnection can start.
   */
  private ping(): void {
    const socket = this.socket;
    if (!socket) return;
    if (typeof socket.ping === "function") {
      try {
        socket.ping();
      } catch (error) {
        this.emit("error", asError(error));
      }
      return;
    }
    this.request({ t: "ping" }).catch((error: unknown) => {
      if (error instanceof LinkProtocolError) return; // An err reply proves the socket is live.
      if (socket !== this.socket) return; // Closed meanwhile; onClose has handled it.
      this.emit("error", asError(error));
      this.abandonSocket(socket, "keepalive failed");
    });
  }
}

/** Normalises whatever the socket handed over into bytes that can be decoded. */
function toBytes(data: unknown): Uint8Array | null {
  if (data instanceof Uint8Array) return data;
  if (data instanceof ArrayBuffer) return new Uint8Array(data);
  if (ArrayBuffer.isView(data)) {
    return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  }
  return null;
}
