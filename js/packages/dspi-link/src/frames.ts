/*
 * Binary data-plane frames, DSPi Link protocol spec section 8.
 *
 * Every frame starts with the same 4-byte header [type u8][flags u8][tag u16].
 * All multi-byte fields are little-endian, matching the firmware. The codec
 * never interprets a command payload: it is the device's bytes, verbatim.
 */

/** Frame type byte (spec 8, table). */
export const FrameType = {
  CMD_REQUEST: 0x01,
  CMD_RESPONSE: 0x81,
  NOTIFY: 0x02,
  POLL: 0x03,
  FWDATA: 0x04,
  FWDATA_ACK: 0x84,
  RESYNC: 0x05,
} as const;

export type FrameTypeValue = (typeof FrameType)[keyof typeof FrameType];

/** CMD response status byte, spec 8.1. 0x00..0x07 are the firmware's CTRL_STATUS_*. */
export enum LinkStatus {
  OK = 0x00,
  BUSY = 0x01,
  ERROR = 0x02,
  BLOCKED = 0x03,
  BULK_LOCKED = 0x04,
  CRC_ERROR = 0x05,
  OVERSIZE = 0x06,
  FRAME_ERROR = 0x07,
  NO_DEVICE = 0x80,
  DENIED = 0x81,
  TIMEOUT = 0x82,
  TOO_LARGE = 0x83,
  RATE_LIMITED = 0x84,
  LOCKED = 0x85,
  UNSUPPORTED = 0x86,
  BAD_FRAME = 0x87,
}

export function statusName(status: number): string {
  return LinkStatus[status] ?? `0x${status.toString(16).padStart(2, "0")}`;
}

/** RESYNC reasons, spec 8.6. An unknown reason is treated as 0 (re-read state). */
export enum ResyncReason {
  NOTIFY_DROPPED = 0,
  DEVICE_REATTACHED = 1,
  CACHE_REBUILT = 2,
}

/**
 * Command direction. The client states the USB transfer type; write-as-read
 * commands travel as `get` exactly as they do over USB (spec 8.2).
 */
export type CmdDirection = "set" | "get";

export const CMD_DIR_SET = 0;
export const CMD_DIR_GET = 1;

export interface CmdRequestFrame {
  type: "cmd_request";
  /** Request id chosen by the client; the response echoes it. */
  tag: number;
  flags?: number;
  handle: number;
  direction: CmdDirection;
  bRequest: number;
  wValue: number;
  /** 2 for application commands; the hub forwards whatever is given. */
  wIndex: number;
  /** SET: payload length. GET: bytes requested. Defaults to payload.length on a SET. */
  wLength?: number;
  payload?: Uint8Array;
}

export interface CmdResponseFrame {
  type: "cmd_response";
  tag: number;
  flags?: number;
  status: LinkStatus | number;
  payload: Uint8Array;
}

export interface NotifyFrame {
  type: "notify";
  /** Link sequence number, per session. A gap means events were missed. */
  tag: number;
  flags?: number;
  handle: number;
  /** Session the hub attributes the change to, 0 when unknown. A hint, not a guarantee. */
  origin: number;
  /** Verbatim v2 notification packet; decode with decodeNotifyPacket. */
  packet: Uint8Array;
}

export interface PollFrame {
  type: "poll";
  tag: number;
  flags?: number;
  handle: number;
  slot: number;
  payload: Uint8Array;
}

export interface FwDataFrame {
  type: "fwdata";
  /** The `xfer` id returned by fw.install. */
  tag: number;
  flags?: number;
  offset: number;
  data: Uint8Array;
}

export interface FwDataAckFrame {
  type: "fwdata_ack";
  tag: number;
  flags?: number;
  offset: number;
  status: number;
}

export interface ResyncFrame {
  type: "resync";
  /** Unused for RESYNC: senders write 0, receivers ignore it (spec 8.6). */
  tag?: number;
  flags?: number;
  handle: number;
  reason: ResyncReason | number;
}

export type LinkFrame =
  | CmdRequestFrame
  | CmdResponseFrame
  | NotifyFrame
  | PollFrame
  | FwDataFrame
  | FwDataAckFrame
  | ResyncFrame;

/** Thrown when a frame of a known type is truncated or self-inconsistent. */
export class LinkFrameError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "LinkFrameError";
  }
}

const HEADER_SIZE = 4;

function view(bytes: Uint8Array): DataView {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
}

function need(bytes: Uint8Array, min: number, what: string): void {
  if (bytes.length < min) {
    throw new LinkFrameError(`${what}: need at least ${min} bytes, got ${bytes.length}`);
  }
}

/** Copy out a slice so the result does not alias the socket's receive buffer. */
function slice(bytes: Uint8Array, start: number, end: number): Uint8Array {
  return bytes.slice(start, Math.min(end, bytes.length));
}

function writeHeader(out: Uint8Array, type: number, flags: number, tag: number): DataView {
  const dv = view(out);
  out[0] = type;
  out[1] = flags & 0xff;
  dv.setUint16(2, tag & 0xffff, true);
  return dv;
}

/** The frame type byte, or null for a buffer too short to have one. */
export function frameTypeOf(bytes: Uint8Array): number | null {
  return bytes.length >= 1 ? bytes[0] : null;
}

/**
 * Decode one binary frame.
 *
 * Returns null for a frame type this version does not know, which per spec 10.4
 * a receiver simply drops. Throws LinkFrameError when a *known* type is
 * malformed, so a caller can tell "not for me" from "broken".
 */
export function decodeFrame(bytes: Uint8Array): LinkFrame | null {
  if (bytes.length < HEADER_SIZE) {
    throw new LinkFrameError(`frame: need at least ${HEADER_SIZE} bytes, got ${bytes.length}`);
  }
  const dv = view(bytes);
  const type = bytes[0];
  const flags = bytes[1];
  const tag = dv.getUint16(2, true);

  switch (type) {
    case FrameType.CMD_REQUEST: {
      need(bytes, 14, "cmd_request");
      const direction: CmdDirection = bytes[5] === CMD_DIR_GET ? "get" : "set";
      const wLength = dv.getUint16(12, true);
      const frame: CmdRequestFrame = {
        type: "cmd_request",
        tag,
        flags,
        handle: bytes[4],
        direction,
        bRequest: bytes[6],
        wValue: dv.getUint16(8, true),
        wIndex: dv.getUint16(10, true),
        wLength,
      };
      // On a GET, wLength is the number of bytes requested, not a payload length.
      if (direction === "set") frame.payload = slice(bytes, 14, 14 + wLength);
      return frame;
    }
    case FrameType.CMD_RESPONSE: {
      need(bytes, 8, "cmd_response");
      const length = dv.getUint16(6, true);
      return {
        type: "cmd_response",
        tag,
        flags,
        status: bytes[4],
        payload: slice(bytes, 8, 8 + length),
      };
    }
    case FrameType.NOTIFY: {
      need(bytes, 8, "notify");
      return {
        type: "notify",
        tag,
        flags,
        handle: bytes[4],
        origin: dv.getUint16(6, true),
        packet: slice(bytes, 8, bytes.length),
      };
    }
    case FrameType.POLL: {
      need(bytes, 8, "poll");
      const length = dv.getUint16(6, true);
      return {
        type: "poll",
        tag,
        flags,
        handle: bytes[4],
        slot: bytes[5],
        payload: slice(bytes, 8, 8 + length),
      };
    }
    case FrameType.FWDATA: {
      need(bytes, 8, "fwdata");
      return {
        type: "fwdata",
        tag,
        flags,
        offset: dv.getUint32(4, true),
        data: slice(bytes, 8, bytes.length),
      };
    }
    case FrameType.FWDATA_ACK: {
      need(bytes, 9, "fwdata_ack");
      return {
        type: "fwdata_ack",
        tag,
        flags,
        offset: dv.getUint32(4, true),
        status: bytes[8],
      };
    }
    case FrameType.RESYNC: {
      need(bytes, 6, "resync");
      return { type: "resync", tag, flags, handle: bytes[4], reason: bytes[5] };
    }
    default:
      return null;
  }
}

/** Encode any frame back to the wire. Round-trips decodeFrame byte for byte. */
export function encodeFrame(frame: LinkFrame): Uint8Array {
  const flags = frame.flags ?? 0;
  switch (frame.type) {
    case "cmd_request": {
      const payload = frame.direction === "set" ? (frame.payload ?? new Uint8Array(0)) : undefined;
      const wLength = frame.wLength ?? payload?.length ?? 0;
      const out = new Uint8Array(14 + (payload?.length ?? 0));
      const dv = writeHeader(out, FrameType.CMD_REQUEST, flags, frame.tag);
      out[4] = frame.handle & 0xff;
      out[5] = frame.direction === "get" ? CMD_DIR_GET : CMD_DIR_SET;
      out[6] = frame.bRequest & 0xff;
      out[7] = 0;
      dv.setUint16(8, frame.wValue & 0xffff, true);
      dv.setUint16(10, frame.wIndex & 0xffff, true);
      dv.setUint16(12, wLength & 0xffff, true);
      if (payload && payload.length) out.set(payload, 14);
      return out;
    }
    case "cmd_response": {
      const out = new Uint8Array(8 + frame.payload.length);
      const dv = writeHeader(out, FrameType.CMD_RESPONSE, flags, frame.tag);
      out[4] = frame.status & 0xff;
      out[5] = 0;
      dv.setUint16(6, frame.payload.length, true);
      out.set(frame.payload, 8);
      return out;
    }
    case "notify": {
      const out = new Uint8Array(8 + frame.packet.length);
      const dv = writeHeader(out, FrameType.NOTIFY, flags, frame.tag);
      out[4] = frame.handle & 0xff;
      out[5] = 0;
      dv.setUint16(6, frame.origin & 0xffff, true);
      out.set(frame.packet, 8);
      return out;
    }
    case "poll": {
      const out = new Uint8Array(8 + frame.payload.length);
      const dv = writeHeader(out, FrameType.POLL, flags, frame.tag);
      out[4] = frame.handle & 0xff;
      out[5] = frame.slot & 0xff;
      dv.setUint16(6, frame.payload.length, true);
      out.set(frame.payload, 8);
      return out;
    }
    case "fwdata": {
      const out = new Uint8Array(8 + frame.data.length);
      const dv = writeHeader(out, FrameType.FWDATA, flags, frame.tag);
      dv.setUint32(4, frame.offset >>> 0, true);
      out.set(frame.data, 8);
      return out;
    }
    case "fwdata_ack": {
      const out = new Uint8Array(9);
      const dv = writeHeader(out, FrameType.FWDATA_ACK, flags, frame.tag);
      dv.setUint32(4, frame.offset >>> 0, true);
      out[8] = frame.status & 0xff;
      return out;
    }
    case "resync": {
      const out = new Uint8Array(6);
      writeHeader(out, FrameType.RESYNC, flags, frame.tag ?? 0);
      out[4] = frame.handle & 0xff;
      out[5] = frame.reason & 0xff;
      return out;
    }
    default: {
      const unknown = frame as { type: string };
      throw new LinkFrameError(`cannot encode frame type ${unknown.type}`);
    }
  }
}
