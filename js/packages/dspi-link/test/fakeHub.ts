/*
 * A hub small enough to live in a test: a WebSocket stand-in that scripts the
 * handshake and answers the few message types the client sends. Nothing here
 * is a network; everything is delivered on the next macrotask so the ordering
 * matches a real socket.
 */

import { decodeFrame, encodeFrame, LinkStatus, type LinkFrame } from "../src/frames.js";
import type { DeviceInfo } from "../src/messages.js";
import type { WebSocketInstance, WebSocketLike } from "../src/client.js";

export interface FakeHubConfig {
  auth?: "none" | "pin";
  pin?: string;
  validToken?: string;
  issuedToken?: string;
  session?: number;
  role?: "viewer" | "control" | "admin";
  devices?: DeviceInfo[];
  caps?: string[];
  /** Answer a decoded CMD request; return null to answer nothing. */
  onCommand?: (frame: Extract<LinkFrame, { type: "cmd_request" }>) => LinkFrame | null;
  snapshot?: Record<string, unknown>;
  /** Sent in hello when `auth` is "pin". */
  pinSource?: "window" | "fixed";
  /**
   * Answers a request the switch below does not know, such as the admin
   * messages: return the reply's body without `id`, or null for unknown_type.
   */
  onRequest?: (msg: Record<string, unknown>) => Record<string, unknown> | null;
}

const soon = (fn: () => void) => setTimeout(fn, 0);

export class FakeHub {
  readonly sockets: FakeSocket[] = [];
  readonly config: Required<Pick<FakeHubConfig, "auth" | "session" | "role">> & FakeHubConfig;
  /** Every text frame the client sent, in order, decoded. */
  readonly received: Array<Record<string, unknown>> = [];
  readonly receivedFrames: LinkFrame[] = [];

  constructor(config: FakeHubConfig = {}) {
    this.config = {
      auth: config.auth ?? "pin",
      session: config.session ?? 12,
      role: config.role ?? "control",
      ...config,
    };
  }

  get socket(): FakeSocket {
    const s = this.sockets[this.sockets.length - 1];
    if (!s) throw new Error("no socket connected");
    return s;
  }

  /** A WebSocket-shaped constructor bound to this hub. */
  get webSocket(): WebSocketLike {
    const hub = this;
    return class extends FakeSocket {
      constructor(url: string, protocols?: string | string[]) {
        super(hub, url, protocols);
      }
    } as unknown as WebSocketLike;
  }

  handleText(socket: FakeSocket, text: string): void {
    const msg = JSON.parse(text) as Record<string, unknown>;
    this.received.push(msg);
    const id = msg.id as number | undefined;

    switch (msg.t) {
      case "hello":
        socket.deliverText({
          t: "hello",
          proto: { major: 1, minor: 0 },
          hub: { id: "6f1a2b3c-4d5e-4f60-8a9b-0c1d2e3f4a5b", name: "Test Hub", kind: "console", version: "1.1.7" },
          auth: this.config.auth,
          ...(this.config.pinSource ? { pin_source: this.config.pinSource } : {}),
          caps: this.config.caps ?? ["cmd", "notify", "poll", "snapshot", "lock", "rename"],
          limits: {
            max_frame: 65536,
            max_payload: 8192,
            max_inflight: 8,
            poll_max_hz: 20,
            poll_budget_bps: 200000,
          },
        });
        return;
      case "auth.token": {
        if (this.config.auth === "pin" && msg.token !== this.config.validToken) {
          socket.deliverText({ t: "err", id, code: "denied", msg: "unknown token" });
          return;
        }
        socket.deliverText({ t: "ok", id, session: this.config.session, role: this.config.role, policy: { denied: [] } });
        return;
      }
      case "auth.pair": {
        if (this.config.auth === "pin" && this.config.pin && msg.pin !== this.config.pin) {
          socket.deliverText({ t: "err", id, code: "denied", msg: "bad pin" });
          return;
        }
        const reply: Record<string, unknown> = {
          t: "ok",
          id,
          session: this.config.session,
          role: this.config.role,
          policy: { denied: [] },
        };
        // Open access issues no token; a pin hub does.
        if (this.config.auth === "pin") reply.token = this.config.issuedToken ?? "TOKEN-abc";
        socket.deliverText(reply);
        return;
      }
      case "device.list":
        socket.deliverText({ t: "ok", id, devices: this.config.devices ?? [] });
        return;
      case "device.snapshot":
        socket.deliverText({ t: "ok", id, handle: msg.handle, ...(this.config.snapshot ?? {}) });
        return;
      case "poll.subscribe": {
        const polls = (msg.polls ?? []) as Array<{ slot: number; hz: number }>;
        socket.deliverText({ t: "ok", id, granted: polls.map((p) => ({ slot: p.slot, hz: p.hz })) });
        return;
      }
      case "poll.unsubscribe":
      case "lock.acquire":
      case "lock.release":
      case "device.rename":
        socket.deliverText({ t: "ok", id });
        return;
      default: {
        const reply = this.config.onRequest?.(msg) ?? { t: "err", code: "unknown_type" };
        socket.deliverText({ ...reply, id });
        return;
      }
    }
  }

  handleBinary(socket: FakeSocket, bytes: Uint8Array): void {
    const frame = decodeFrame(bytes);
    if (!frame) return;
    this.receivedFrames.push(frame);
    if (frame.type !== "cmd_request") return;
    const reply = this.config.onCommand
      ? this.config.onCommand(frame)
      : ({ type: "cmd_response", tag: frame.tag, status: LinkStatus.OK, payload: new Uint8Array(0) } as LinkFrame);
    if (reply) socket.deliverFrame(reply);
  }
}

export class FakeSocket implements WebSocketInstance {
  binaryType = "blob";
  readyState = 0;
  onopen: ((ev: unknown) => void) | null = null;
  onclose: ((ev: { code?: number; reason?: string }) => void) | null = null;
  onerror: ((ev: unknown) => void) | null = null;
  onmessage: ((ev: { data: unknown }) => void) | null = null;

  readonly sentText: string[] = [];
  readonly sentBinary: Uint8Array[] = [];

  constructor(
    readonly hub: FakeHub,
    readonly url: string,
    readonly protocols?: string | string[],
  ) {
    hub.sockets.push(this);
    soon(() => {
      this.readyState = 1;
      this.onopen?.({});
    });
  }

  send(data: string | ArrayBufferLike | ArrayBufferView): void {
    if (typeof data === "string") {
      this.sentText.push(data);
      soon(() => this.hub.handleText(this, data));
      return;
    }
    const bytes =
      data instanceof Uint8Array
        ? data
        : ArrayBuffer.isView(data)
          ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
          : new Uint8Array(data as ArrayBuffer);
    const copy = bytes.slice();
    this.sentBinary.push(copy);
    soon(() => this.hub.handleBinary(this, copy));
  }

  close(code = 1000, reason = ""): void {
    if (this.readyState === 3) return;
    this.readyState = 3;
    soon(() => this.onclose?.({ code, reason }));
  }

  deliverText(msg: unknown): void {
    soon(() => this.onmessage?.({ data: JSON.stringify(msg) }));
  }

  deliverFrame(frame: LinkFrame): void {
    this.deliverBytes(encodeFrame(frame));
  }

  deliverBytes(bytes: Uint8Array): void {
    const buf = bytes.slice().buffer;
    soon(() => this.onmessage?.({ data: buf }));
  }

  /** Close from the hub's end, e.g. code 4002 for a revoked token. */
  serverClose(code: number, reason = ""): void {
    this.readyState = 3;
    soon(() => this.onclose?.({ code, reason }));
  }
}

/** Let queued macrotasks run; the fake delivers everything on setTimeout(0). */
export function settle(rounds = 8): Promise<void> {
  return new Promise((resolve) => {
    let n = rounds;
    const step = () => (n-- > 0 ? setTimeout(step, 0) : resolve());
    step();
  });
}
