/*
 * LinkClient: one authenticated WebSocket session to a DSPi Link hub.
 *
 * Owns the handshake (spec 5), request/reply correlation on both planes, the
 * device inventory, poll subscriptions and reconnection. It does not interpret
 * command payloads; those are the device's bytes and the caller's business.
 */

import {
  type CmdRequestFrame,
  type CmdResponseFrame,
  type NotifyFrame,
  type PollFrame,
  type ResyncFrame,
  LinkFrameError,
  LinkStatus,
  decodeFrame,
  encodeFrame,
  statusName,
} from "./frames.js";
import {
  type AuthPairMessage,
  type AuthTokenMessage,
  type DeviceInfo,
  type ErrMessage,
  type HelloHubMessage,
  type HubLimits,
  type LinkCapability,
  type LinkErrorCode,
  type LinkMessage,
  type LinkRole,
  type OkMessage,
  type PollGrant,
  type PollRequest,
  type PolicyDescriptor,
  decode as decodeMessage,
  encode as encodeMessage,
} from "./messages.js";
import { base64ToBytes } from "./decoders.js";

export const LINK_SUBPROTOCOL = "dspi-link-1";
export const LINK_DEFAULT_PORT = 11915;
export const LINK_WS_PATH = "/dspi/v1";

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

/** Why the socket closed, for the codes hubs use (spec 5). */
export type LinkCloseReason =
  | "normal"
  | "protocol_error"
  | "auth_failed"
  | "revoked"
  | "shutting_down"
  | "abnormal";

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

export interface LinkCloseInfo {
  code: number;
  reason: LinkCloseReason;
  /** True when the client will try to reconnect. */
  willReconnect: boolean;
  message?: string;
}

export interface LinkClientOptions {
  /** WebSocket implementation, for Node versions or tests without a global one. */
  webSocket?: WebSocketLike;
  /** Reconnect after an unexpected close. Off by default so tests stay deterministic. */
  reconnect?: boolean;
  /** Backoff cap, spec 5. Delays run 1, 2, 4, ... up to this. */
  reconnectMaxMs?: number;
  /** How long a tunnelled command may take before it rejects. */
  commandTimeoutMs?: number;
  /** How long a control-plane request may take before it rejects. */
  requestTimeoutMs?: number;
  /** Keepalive period. 0 disables it. */
  pingIntervalMs?: number;
}

export interface ConnectOptions {
  /** Name the hub shows in its client list. */
  clientName: string;
  /** Stored pairing token, if this client has one for this hub. */
  token?: string;
  app?: string;
  version?: string;
  /** Role to ask for when pairing. */
  role?: LinkRole;
}

/** The subset of the WebSocket constructor this client needs. */
export type WebSocketLike = new (url: string, protocols?: string | string[]) => WebSocketInstance;

export interface WebSocketInstance {
  binaryType?: string;
  readyState: number;
  send(data: string | ArrayBufferLike | ArrayBufferView): void;
  close(code?: number, reason?: string): void;
  onopen: ((ev: unknown) => void) | null;
  onclose: ((ev: { code?: number; reason?: string }) => void) | null;
  onerror: ((ev: unknown) => void) | null;
  onmessage: ((ev: { data: unknown }) => void) | null;
  /** Present on Node's `ws`; absent in browsers, which cannot send control pings. */
  ping?: () => void;
}

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

/** A control-plane `err` reply turned into a rejection. */
export class LinkProtocolError extends Error {
  readonly code: LinkErrorCode;
  constructor(code: LinkErrorCode, message?: string) {
    super(message ? `${code}: ${message}` : code);
    this.name = "LinkProtocolError";
    this.code = code;
  }
}

/** A CMD response whose status was not OK, for callers that want a throw. */
export class LinkCommandError extends Error {
  readonly status: number;
  readonly response: CmdResponseFrame;
  constructor(response: CmdResponseFrame) {
    super(`command failed: ${statusName(response.status)}`);
    this.name = "LinkCommandError";
    this.status = response.status;
    this.response = response;
  }
}

export interface LinkClientEventMap {
  state: { state: LinkClientState; previous: LinkClientState };
  devices: DeviceInfo[];
  notify: NotifyFrame;
  poll: PollFrame;
  resync: ResyncFrame;
  message: LinkMessage;
  error: Error;
  close: LinkCloseInfo;
}

type Listener<K extends keyof LinkClientEventMap> = (payload: LinkClientEventMap[K]) => void;

interface PendingRequest {
  resolve: (msg: OkMessage) => void;
  reject: (err: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

interface PendingCommand {
  resolve: (frame: CmdResponseFrame) => void;
  reject: (err: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

const RECONNECT_BASE_MS = 1000;

export class LinkClient {
  private ws: WebSocketInstance | null = null;
  private readonly WS: WebSocketLike;
  private readonly options: Required<Omit<LinkClientOptions, "webSocket">>;

  private url = "";
  private connectOptions: ConnectOptions = { clientName: "DSPi client" };

  private _state: LinkClientState = "idle";
  private _hub: HelloHubMessage | null = null;
  private _session: number | null = null;
  private _role: LinkRole | null = null;
  private _policy: PolicyDescriptor | null = null;
  private _token: string | null = null;
  private devices = new Map<number, DeviceInfo>();

  private nextId = 1;
  private nextTag = 1;
  private pendingRequests = new Map<number, PendingRequest>();
  private pendingCommands = new Map<number, PendingCommand>();

  /** Remembered so a reconnect restores the same meter feeds. */
  private pollSubscriptions = new Map<number, Map<number, PollRequest>>();

  private listeners = new Map<string, Set<(payload: never) => void>>();

  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private reconnectAttempt = 0;
  private manualClose = false;
  private handshake: { resolve: () => void; reject: (e: Error) => void } | null = null;

  constructor(opts: LinkClientOptions = {}) {
    const impl = opts.webSocket ?? (globalThis as { WebSocket?: WebSocketLike }).WebSocket;
    if (!impl) {
      throw new Error("no WebSocket available; pass options.webSocket");
    }
    this.WS = impl;
    this.options = {
      reconnect: opts.reconnect ?? false,
      reconnectMaxMs: opts.reconnectMaxMs ?? 30000,
      commandTimeoutMs: opts.commandTimeoutMs ?? 10000,
      requestTimeoutMs: opts.requestTimeoutMs ?? 10000,
      pingIntervalMs: opts.pingIntervalMs ?? 15000,
    };
  }

  // -- accessors ------------------------------------------------------------

  get state(): LinkClientState {
    return this._state;
  }

  /** The hub's hello, once it has arrived. */
  get hub(): HelloHubMessage | null {
    return this._hub;
  }

  get hubId(): string | null {
    return this._hub?.hub.id ?? null;
  }

  /** Session id the hub stamps into NOTIFY frames as `origin`. */
  get session(): number | null {
    return this._session;
  }

  get role(): LinkRole | null {
    return this._role;
  }

  get policy(): PolicyDescriptor | null {
    return this._policy;
  }

  /** The token from the last successful pairing, if the hub issued one. */
  get token(): string | null {
    return this._token;
  }

  get caps(): LinkCapability[] {
    return this._hub?.caps ?? [];
  }

  get limits(): HubLimits | null {
    return this._hub?.limits ?? null;
  }

  hasCap(name: LinkCapability): boolean {
    return this.caps.includes(name);
  }

  get deviceList(): DeviceInfo[] {
    return [...this.devices.values()].sort((a, b) => a.handle - b.handle);
  }

  device(handle: number): DeviceInfo | undefined {
    return this.devices.get(handle);
  }

  // -- events ---------------------------------------------------------------

  on<K extends keyof LinkClientEventMap>(event: K, fn: Listener<K>): () => void {
    let set = this.listeners.get(event as string);
    if (!set) {
      set = new Set();
      this.listeners.set(event as string, set);
    }
    set.add(fn as (payload: never) => void);
    return () => this.off(event, fn);
  }

  off<K extends keyof LinkClientEventMap>(event: K, fn: Listener<K>): void {
    this.listeners.get(event as string)?.delete(fn as (payload: never) => void);
  }

  private emit<K extends keyof LinkClientEventMap>(event: K, payload: LinkClientEventMap[K]): void {
    const set = this.listeners.get(event as string);
    if (!set) return;
    for (const fn of [...set]) {
      try {
        (fn as Listener<K>)(payload);
      } catch (e) {
        // A listener throwing must not tear down the socket loop.
        if (event !== "error") this.emit("error", e as Error);
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
   * Open a session. Resolves once the handshake settles: `ready` when the hub
   * accepted us, `awaiting_pairing` when it wants a PIN (call `pair`).
   */
  connect(url: string, opts: ConnectOptions): Promise<void> {
    this.url = url;
    this.connectOptions = opts;
    this._token = opts.token ?? null;
    this.manualClose = false;
    this.reconnectAttempt = 0;
    return this.open();
  }

  private open(): Promise<void> {
    this.cancelReconnect();
    this.setState("connecting");
    return new Promise<void>((resolve, reject) => {
      this.handshake = { resolve, reject };
      let ws: WebSocketInstance;
      try {
        ws = new this.WS(this.url, LINK_SUBPROTOCOL);
      } catch (e) {
        this.handshake = null;
        this.setState("closed");
        reject(e as Error);
        return;
      }
      this.ws = ws;
      try {
        ws.binaryType = "arraybuffer";
      } catch {
        // Some implementations only allow this after open; harmless either way.
      }
      ws.onopen = () => this.onOpen();
      ws.onmessage = (ev) => this.onMessage(ev.data);
      ws.onerror = () => this.emit("error", new Error("websocket error"));
      ws.onclose = (ev) => this.onClose(ev?.code ?? 1006, ev?.reason);
    });
  }

  /** Close the session and stop reconnecting. */
  disconnect(code = 1000, reason = "client closing"): void {
    this.manualClose = true;
    this.cancelReconnect();
    this.stopPing();
    const ws = this.ws;
    this.ws = null;
    try {
      ws?.close(code, reason);
    } catch {
      // Already closing; nothing useful to do.
    }
    this.failPending(new Error("disconnected"));
    this.setState("closed");
  }

  private onOpen(): void {
    this.setState("handshaking");
    this.send({
      t: "hello",
      proto: { major: 1, minor: 1 },
      client: {
        name: this.connectOptions.clientName,
        app: this.connectOptions.app ?? "dspi-link-js",
        version: this.connectOptions.version ?? "1.1.0",
      },
    });
  }

  private onClose(code: number, message?: string): void {
    this.stopPing();
    this.ws = null;
    const reason = closeReasonFor(code);
    // A revoked token or a rejected auth will fail the same way on retry, so
    // those close codes end the session instead of starting a backoff loop.
    const retryable = reason !== "normal" && reason !== "auth_failed" && reason !== "revoked";
    const willReconnect = this.options.reconnect && !this.manualClose && retryable;

    this.failPending(new Error(`connection closed (${code})`));
    if (reason === "revoked") this._token = null;
    this.emit("close", { code, reason, willReconnect, message });

    if (this.handshake) {
      const { reject } = this.handshake;
      this.handshake = null;
      reject(new Error(`connection closed during handshake (${code})`));
    }

    if (willReconnect) {
      this.setState("reconnecting");
      this.scheduleReconnect();
    } else {
      this.setState("closed");
    }
  }

  private scheduleReconnect(): void {
    const delay = Math.min(
      RECONNECT_BASE_MS * 2 ** this.reconnectAttempt,
      this.options.reconnectMaxMs,
    );
    this.reconnectAttempt++;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.open().catch((e) => this.emit("error", e as Error));
    }, delay);
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
    let msg: LinkMessage;
    try {
      msg = decodeMessage(text);
    } catch (e) {
      this.emit("error", e as Error);
      return;
    }
    this.emit("message", msg);

    switch (msg.t) {
      case "hello":
        this.onHubHello(msg as HelloHubMessage);
        return;
      case "ok":
      case "err":
        this.settleRequest(msg as OkMessage | ErrMessage);
        return;
      case "device.added":
      case "device.changed": {
        const dev = (msg as { device?: DeviceInfo }).device;
        if (dev) {
          this.devices.set(dev.handle, dev);
          this.emit("devices", this.deviceList);
        }
        return;
      }
      case "device.removed": {
        const handle = (msg as { handle?: number }).handle;
        if (handle !== undefined && this.devices.delete(handle)) {
          this.pollSubscriptions.delete(handle);
          this.emit("devices", this.deviceList);
        }
        return;
      }
      default:
        // Unknown events are dropped, per spec 10.3; `message` already fired.
        return;
    }
  }

  private onBinary(bytes: Uint8Array): void {
    let frame;
    try {
      frame = decodeFrame(bytes);
    } catch (e) {
      if (e instanceof LinkFrameError) this.emit("error", e);
      return;
    }
    if (!frame) return; // Unknown frame type: drop it (spec 10.4).

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
        this.emit("notify", frame);
        return;
      case "poll":
        this.emit("poll", frame);
        return;
      case "resync":
        this.emit("resync", frame);
        return;
      default:
        return;
    }
  }

  private onHubHello(hello: HelloHubMessage): void {
    this._hub = hello;
    if (hello.auth === "none") {
      // Open access opens the session on hello, but the client still needs its
      // session id to suppress its own notification echoes, and auth.pair is
      // the documented way to ask for it (spec 6.1). The PIN is ignored.
      this.setState("authenticating");
      this.sendPair("000000", this.connectOptions.clientName, this.connectOptions.role).catch(
        (e) => this.onAuthFailed(e as Error),
      );
      return;
    }
    if (this._token) {
      this.setState("authenticating");
      this.sendToken(this._token).catch((e) => this.onAuthFailed(e as Error));
      return;
    }
    this.setState("awaiting_pairing");
    this.settleHandshake();
  }

  private async sendToken(token: string): Promise<void> {
    const ok = await this.request<AuthTokenMessage>({ t: "auth.token", id: 0, token });
    await this.onAuthenticated(ok);
  }

  /**
   * The automatic half of the handshake could not authenticate. A rejected
   * token is not fatal: drop it and let the user pair again. If the socket has
   * already gone, onClose owns the state and there is nothing to say here.
   */
  private onAuthFailed(e: Error): void {
    this._token = null;
    this.emit("error", e);
    if (!this.ws) return;
    this.setState("awaiting_pairing");
    this.settleHandshake();
  }

  private async sendPair(pin: string, name: string, role?: LinkRole): Promise<string | null> {
    const body: AuthPairMessage = { t: "auth.pair", id: 0, pin, name };
    if (role) body.role = role;
    const ok = await this.request<AuthPairMessage>(body);
    await this.onAuthenticated(ok);
    return this._token;
  }

  private async onAuthenticated(ok: OkMessage): Promise<void> {
    this._session = ok.session ?? null;
    this._role = ok.role ?? null;
    this._policy = ok.policy ?? null;
    if (typeof ok.token === "string") this._token = ok.token;
    this.setState("ready");
    this.reconnectAttempt = 0;
    this.startPing();
    try {
      await this.refreshDevices();
      await this.restorePollSubscriptions();
    } catch (e) {
      this.emit("error", e as Error);
    }
    this.settleHandshake();
  }

  private settleHandshake(): void {
    if (!this.handshake) return;
    const { resolve } = this.handshake;
    this.handshake = null;
    resolve();
  }

  // -- public API -----------------------------------------------------------

  /**
   * Pair with a PIN the hub is showing. Returns the long-lived token to store
   * against the hub id (null when the hub runs open access and issues none).
   */
  pair(pin: string, clientName?: string, role?: LinkRole): Promise<string | null> {
    const name = clientName ?? this.connectOptions.clientName;
    if (clientName) this.connectOptions.clientName = clientName;
    if (role) this.connectOptions.role = role;
    this.setState("authenticating");
    return this.sendPair(pin, name, role ?? this.connectOptions.role).catch((e) => {
      this.setState("awaiting_pairing");
      throw e;
    });
  }

  /** Re-read the inventory and replace the cached device map. */
  async refreshDevices(): Promise<DeviceInfo[]> {
    const ok = await this.request({ t: "device.list", id: 0 });
    this.devices = new Map((ok.devices ?? []).map((d) => [d.handle, d]));
    this.emit("devices", this.deviceList);
    return this.deviceList;
  }

  /** Tunnel one vendor command and wait for its response frame (spec 8.2). */
  command(req: Omit<CmdRequestFrame, "type" | "tag">): Promise<CmdResponseFrame> {
    const ws = this.requireSocket();
    const tag = this.allocateTag();
    const frame: CmdRequestFrame = { ...req, type: "cmd_request", tag };
    return new Promise<CmdResponseFrame>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pendingCommands.delete(tag);
        reject(new Error(`command 0x${req.bRequest.toString(16)} timed out`));
      }, this.options.commandTimeoutMs);
      this.pendingCommands.set(tag, { resolve, reject, timer });
      try {
        ws.send(encodeFrame(frame));
      } catch (e) {
        this.pendingCommands.delete(tag);
        clearTimeout(timer);
        reject(e as Error);
      }
    });
  }

  /** command(), rejecting when the hub or device did not answer OK. */
  async commandOk(req: Omit<CmdRequestFrame, "type" | "tag">): Promise<Uint8Array> {
    const res = await this.command(req);
    if (res.status !== LinkStatus.OK) throw new LinkCommandError(res);
    return res.payload;
  }

  /** Convenience GET: `wLength` is the number of bytes requested. */
  get(
    handle: number,
    bRequest: number,
    wLength: number,
    wValue = 0,
    wIndex = 2,
  ): Promise<CmdResponseFrame> {
    return this.command({ handle, direction: "get", bRequest, wValue, wIndex, wLength });
  }

  /** Convenience SET. */
  set(
    handle: number,
    bRequest: number,
    payload: Uint8Array,
    wValue = 0,
    wIndex = 2,
  ): Promise<CmdResponseFrame> {
    return this.command({ handle, direction: "set", bRequest, wValue, wIndex, payload });
  }

  /** The hub's cached bulk blob and status frame, one round trip (spec 7.5). */
  async snapshot(handle: number): Promise<SnapshotResult> {
    const ok = await this.request({ t: "device.snapshot", id: 0, handle });
    const result: SnapshotResult = {
      handle: ok.handle ?? handle,
      wire_version: ok.wire_version,
      age_ms: ok.age_ms,
      bulk_b64: ok.bulk_b64,
      status_b64: ok.status_b64,
    };
    if (ok.bulk_b64) result.bulk = base64ToBytes(ok.bulk_b64);
    if (ok.status_b64) result.status = base64ToBytes(ok.status_b64);
    return result;
  }

  /** Subscribe to periodic GETs the hub pushes back as POLL frames (spec 7.6). */
  async subscribePolls(handle: number, polls: PollRequest[]): Promise<PollGrant[]> {
    const ok = await this.request({ t: "poll.subscribe", id: 0, handle, polls });
    let slots = this.pollSubscriptions.get(handle);
    if (!slots) {
      slots = new Map();
      this.pollSubscriptions.set(handle, slots);
    }
    for (const p of polls) slots.set(p.slot, p);
    return ok.granted ?? [];
  }

  async unsubscribePolls(handle: number, slots: number[]): Promise<void> {
    const known = this.pollSubscriptions.get(handle);
    if (known) for (const s of slots) known.delete(s);
    await this.request({ t: "poll.unsubscribe", id: 0, handle, slots });
  }

  private async restorePollSubscriptions(): Promise<void> {
    for (const [handle, slots] of this.pollSubscriptions) {
      if (slots.size === 0) continue;
      await this.request({ t: "poll.subscribe", id: 0, handle, polls: [...slots.values()] });
    }
  }

  async acquireLock(handle: number, reason?: string, timeoutMs?: number): Promise<void> {
    const msg: Record<string, unknown> = { t: "lock.acquire", id: 0, handle };
    if (reason) msg.reason = reason;
    if (timeoutMs !== undefined) msg.timeout_ms = timeoutMs;
    await this.request(msg as unknown as LinkMessage);
  }

  async releaseLock(handle: number): Promise<void> {
    await this.request({ t: "lock.release", id: 0, handle });
  }

  async renameDevice(handle: number, name: string): Promise<void> {
    await this.request({ t: "device.rename", id: 0, handle, name });
  }

  // -- plumbing -------------------------------------------------------------

  /** Send a request and wait for its ok/err. `id` is assigned here. */
  request<T extends LinkMessage = LinkMessage>(msg: T | Record<string, unknown>): Promise<OkMessage> {
    const ws = this.requireSocket();
    const id = this.nextId++;
    if (this.nextId > 0x7fffffff) this.nextId = 1;
    const framed = { ...(msg as Record<string, unknown>), id } as LinkMessage;
    return new Promise<OkMessage>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pendingRequests.delete(id);
        reject(new Error(`request ${String((msg as { t: string }).t)} timed out`));
      }, this.options.requestTimeoutMs);
      this.pendingRequests.set(id, { resolve, reject, timer });
      try {
        ws.send(encodeMessage(framed));
      } catch (e) {
        this.pendingRequests.delete(id);
        clearTimeout(timer);
        reject(e as Error);
      }
    });
  }

  private settleRequest(msg: OkMessage | ErrMessage): void {
    const id = msg.id;
    if (id === undefined) return;
    const pending = this.pendingRequests.get(id);
    if (!pending) return;
    this.pendingRequests.delete(id);
    clearTimeout(pending.timer);
    if (msg.t === "ok") pending.resolve(msg as OkMessage);
    else pending.reject(new LinkProtocolError((msg as ErrMessage).code, (msg as ErrMessage).msg));
  }

  private send(msg: LinkMessage): void {
    this.requireSocket().send(encodeMessage(msg));
  }

  private requireSocket(): WebSocketInstance {
    if (!this.ws) throw new Error("not connected");
    return this.ws;
  }

  /** u16 request ids, skipping 0 and anything still outstanding. */
  private allocateTag(): number {
    for (let i = 0; i < 0x10000; i++) {
      const tag = this.nextTag;
      this.nextTag = this.nextTag >= 0xffff ? 1 : this.nextTag + 1;
      if (!this.pendingCommands.has(tag)) return tag;
    }
    throw new Error("no free command tag");
  }

  private failPending(err: Error): void {
    for (const [, p] of this.pendingRequests) {
      clearTimeout(p.timer);
      p.reject(err);
    }
    this.pendingRequests.clear();
    for (const [, p] of this.pendingCommands) {
      clearTimeout(p.timer);
      p.reject(err);
    }
    this.pendingCommands.clear();
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
   * implementation offers one (Node's `ws`) we use it, and otherwise we send a
   * cheap request instead. The hub answers `unknown_type`, which is a perfectly
   * good round trip: it proves the socket is not half open.
   */
  private ping(): void {
    const ws = this.ws;
    if (!ws) return;
    if (typeof ws.ping === "function") {
      try {
        ws.ping();
      } catch (e) {
        this.emit("error", e as Error);
      }
      return;
    }
    this.request({ t: "ping" }).catch((e) => {
      if (e instanceof LinkProtocolError) return; // err reply is still a live socket
      this.emit("error", e as Error);
    });
  }
}

/** Normalise whatever the socket handed us into bytes we can decode. */
function toBytes(data: unknown): Uint8Array | null {
  if (data instanceof Uint8Array) return data;
  if (data instanceof ArrayBuffer) return new Uint8Array(data);
  if (ArrayBuffer.isView(data)) {
    const v = data as ArrayBufferView;
    return new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
  }
  return null;
}
