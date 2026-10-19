/*
 * Binary data-plane frames: the codec for Link spec 8 (8.1 to 8.6).
 *
 * Every frame starts with the same 4-byte header [type u8][flags u8][tag u16].
 * All multi-byte fields are little-endian, matching the firmware. The codec
 * is separate from the client so hubs, tests and tools can share it; it never
 * interprets a command payload, which is the device's bytes, verbatim.
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

/**
 * A status byte's name, for messages and logs.
 *
 * @param status A CMD response status (Link spec 8.1).
 * @returns The LinkStatus name, such as "LOCKED", or the byte in hex.
 */
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

/** The `dir` byte of a CMD request for a SET (host to device). */
export const CMD_DIR_SET = 0;
/** The `dir` byte of a CMD request for a GET (device to host). */
export const CMD_DIR_GET = 1;

/** The 4-byte header every frame starts with. */
const HEADER_BYTES = 4;
/** CMD request header: handle, dir, bRequest, reserved, wValue, wIndex, wLength. */
const CMD_REQUEST_HEADER_BYTES = 14;
/** CMD response, NOTIFY and POLL headers: the frame header and four bytes of fields. */
const CMD_RESPONSE_HEADER_BYTES = 8;
const NOTIFY_HEADER_BYTES = 8;
const POLL_HEADER_BYTES = 8;
/** FWDATA header: the frame header and the u32 offset. A chunk carries up to max_frame minus this. */
export const FWDATA_HEADER_BYTES = 8;
/** FWDATA ack: the frame header, the u32 offset and the status byte. */
const FWDATA_ACK_BYTES = 9;
/** RESYNC: the frame header, handle and reason. */
const RESYNC_BYTES = 6;

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
  /** The `xfer` id returned by fw.install or hub.update. */
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
  /** @param message What is wrong with the frame. */
  constructor(message: string) {
    super(message);
    this.name = "LinkFrameError";
  }
}

function dataViewOf(bytes: Uint8Array): DataView {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
}

function requireLength(bytes: Uint8Array, minimum: number, what: string): void {
  if (bytes.length < minimum) {
    throw new LinkFrameError(`${what}: need at least ${minimum} bytes, got ${bytes.length}`);
  }
}

/** Copy out a slice so the result does not alias the socket's receive buffer. */
function slice(bytes: Uint8Array, start: number, end: number): Uint8Array {
  return bytes.slice(start, Math.min(end, bytes.length));
}

function writeHeader(out: Uint8Array, type: number, flags: number, tag: number): DataView {
  const view = dataViewOf(out);
  out[0] = type;
  out[1] = flags & 0xff;
  view.setUint16(2, tag & 0xffff, true);
  return view;
}

/**
 * Reads a frame's type without decoding the rest.
 *
 * @param bytes A binary frame.
 * @returns The type byte, or null for an empty buffer.
 */
export function frameTypeOf(bytes: Uint8Array): number | null {
  return bytes.length >= 1 ? bytes[0] : null;
}

/**
 * Decodes one binary frame.
 *
 * A frame type this version does not know comes back as null, which a
 * receiver simply drops (Link spec 10, rule 4). A malformed frame of a *known*
 * type throws, so a caller can tell "not for me" from "broken".
 *
 * @param bytes One whole binary WebSocket message.
 * @returns The decoded frame, or null for an unknown type.
 * @throws LinkFrameError when a known frame is truncated.
 */
export function decodeFrame(bytes: Uint8Array): LinkFrame | null {
  requireLength(bytes, HEADER_BYTES, "frame");
  const view = dataViewOf(bytes);
  const type = bytes[0];
  const flags = bytes[1];
  const tag = view.getUint16(2, true);

  switch (type) {
    case FrameType.CMD_REQUEST: {
      requireLength(bytes, CMD_REQUEST_HEADER_BYTES, "cmd_request");
      const direction: CmdDirection = bytes[5] === CMD_DIR_GET ? "get" : "set";
      const wLength = view.getUint16(12, true);
      const frame: CmdRequestFrame = {
        type: "cmd_request",
        tag,
        flags,
        handle: bytes[4],
        direction,
        bRequest: bytes[6],
        wValue: view.getUint16(8, true),
        wIndex: view.getUint16(10, true),
        wLength,
      };
      // On a GET, wLength is the number of bytes requested, not a payload length.
      if (direction === "set") frame.payload = slice(bytes, CMD_REQUEST_HEADER_BYTES, CMD_REQUEST_HEADER_BYTES + wLength);
      return frame;
    }
    case FrameType.CMD_RESPONSE: {
      requireLength(bytes, CMD_RESPONSE_HEADER_BYTES, "cmd_response");
      const length = view.getUint16(6, true);
      return {
        type: "cmd_response",
        tag,
        flags,
        status: bytes[4],
        payload: slice(bytes, CMD_RESPONSE_HEADER_BYTES, CMD_RESPONSE_HEADER_BYTES + length),
      };
    }
    case FrameType.NOTIFY: {
      requireLength(bytes, NOTIFY_HEADER_BYTES, "notify");
      return {
        type: "notify",
        tag,
        flags,
        handle: bytes[4],
        origin: view.getUint16(6, true),
        packet: slice(bytes, NOTIFY_HEADER_BYTES, bytes.length),
      };
    }
    case FrameType.POLL: {
      requireLength(bytes, POLL_HEADER_BYTES, "poll");
      const length = view.getUint16(6, true);
      return {
        type: "poll",
        tag,
        flags,
        handle: bytes[4],
        slot: bytes[5],
        payload: slice(bytes, POLL_HEADER_BYTES, POLL_HEADER_BYTES + length),
      };
    }
    case FrameType.FWDATA: {
      requireLength(bytes, FWDATA_HEADER_BYTES, "fwdata");
      return {
        type: "fwdata",
        tag,
        flags,
        offset: view.getUint32(4, true),
        data: slice(bytes, FWDATA_HEADER_BYTES, bytes.length),
      };
    }
    case FrameType.FWDATA_ACK: {
      requireLength(bytes, FWDATA_ACK_BYTES, "fwdata_ack");
      return {
        type: "fwdata_ack",
        tag,
        flags,
        offset: view.getUint32(4, true),
        status: bytes[8],
      };
    }
    case FrameType.RESYNC: {
      requireLength(bytes, RESYNC_BYTES, "resync");
      return { type: "resync", tag, flags, handle: bytes[4], reason: bytes[5] };
    }
    default:
      return null;
  }
}

/**
 * Encodes a frame for the wire. Round-trips decodeFrame byte for byte.
 *
 * @param frame The frame to encode; a missing `flags` is 0.
 * @returns The bytes to send as one binary WebSocket message.
 */
export function encodeFrame(frame: LinkFrame): Uint8Array {
  const flags = frame.flags ?? 0;
  switch (frame.type) {
    case "cmd_request": {
      const payload = frame.direction === "set" ? (frame.payload ?? new Uint8Array(0)) : undefined;
      const wLength = frame.wLength ?? payload?.length ?? 0;
      const out = new Uint8Array(CMD_REQUEST_HEADER_BYTES + (payload?.length ?? 0));
      const view = writeHeader(out, FrameType.CMD_REQUEST, flags, frame.tag);
      out[4] = frame.handle & 0xff;
      out[5] = frame.direction === "get" ? CMD_DIR_GET : CMD_DIR_SET;
      out[6] = frame.bRequest & 0xff;
      out[7] = 0;
      view.setUint16(8, frame.wValue & 0xffff, true);
      view.setUint16(10, frame.wIndex & 0xffff, true);
      view.setUint16(12, wLength & 0xffff, true);
      if (payload && payload.length) out.set(payload, CMD_REQUEST_HEADER_BYTES);
      return out;
    }
    case "cmd_response": {
      const out = new Uint8Array(CMD_RESPONSE_HEADER_BYTES + frame.payload.length);
      const view = writeHeader(out, FrameType.CMD_RESPONSE, flags, frame.tag);
      out[4] = frame.status & 0xff;
      out[5] = 0;
      view.setUint16(6, frame.payload.length, true);
      out.set(frame.payload, CMD_RESPONSE_HEADER_BYTES);
      return out;
    }
    case "notify": {
      const out = new Uint8Array(NOTIFY_HEADER_BYTES + frame.packet.length);
      const view = writeHeader(out, FrameType.NOTIFY, flags, frame.tag);
      out[4] = frame.handle & 0xff;
      out[5] = 0;
      view.setUint16(6, frame.origin & 0xffff, true);
      out.set(frame.packet, NOTIFY_HEADER_BYTES);
      return out;
    }
    case "poll": {
      const out = new Uint8Array(POLL_HEADER_BYTES + frame.payload.length);
      const view = writeHeader(out, FrameType.POLL, flags, frame.tag);
      out[4] = frame.handle & 0xff;
      out[5] = frame.slot & 0xff;
      view.setUint16(6, frame.payload.length, true);
      out.set(frame.payload, POLL_HEADER_BYTES);
      return out;
    }
    case "fwdata": {
      const out = new Uint8Array(FWDATA_HEADER_BYTES + frame.data.length);
      const view = writeHeader(out, FrameType.FWDATA, flags, frame.tag);
      view.setUint32(4, frame.offset >>> 0, true);
      out.set(frame.data, FWDATA_HEADER_BYTES);
      return out;
    }
    case "fwdata_ack": {
      const out = new Uint8Array(FWDATA_ACK_BYTES);
      const view = writeHeader(out, FrameType.FWDATA_ACK, flags, frame.tag);
      view.setUint32(4, frame.offset >>> 0, true);
      out[8] = frame.status & 0xff;
      return out;
    }
    case "resync": {
      const out = new Uint8Array(RESYNC_BYTES);
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
