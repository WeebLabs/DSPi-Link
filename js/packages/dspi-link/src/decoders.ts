/*
 * Decoders for the firmware payloads Link carries verbatim.
 *
 * Link itself never interprets these bytes (spec 1.2): a client decodes them
 * with the same code it would use over USB. This module is the v1 surface of
 * that decoding, deliberately small: the bulk header, two bulk accessors, the
 * combined status frame, the platform reply and the v2 notification packet.
 * The whole WireBulkParams struct is out of scope.
 *
 * Every offset below belongs to a specific wire-format version and is only
 * valid while the header's format_version matches. Check it before reading.
 */

/** Wire format this module's bulk offsets were taken from (firmware bulk_params.h). */
export const BULK_WIRE_VERSION = 30;

/** Byte length of WireBulkParams at wire version 30. */
export const BULK_PARAMS_SIZE = 5980;

/** WireHeader, 16 bytes at offset 0. Stable since the wire format began. */
export const BULK_HEADER_SIZE = 16;

/** WireGlobalParams; its first float is preamp_gain_db. Offset 16 since V16. */
export const BULK_GLOBAL_OFFSET = 16;

/** WireMasterVolume.master_volume_db, float32 LE. Offset 4700 at wire version 30. */
export const BULK_MASTER_VOLUME_OFFSET = 4700;

/** master_volume_db sentinel meaning muted (the firmware's mute value). */
export const MASTER_VOLUME_MUTE_DB = -128;

export const WIRE_PLATFORM_RP2040 = 0;
export const WIRE_PLATFORM_RP2350 = 1;

export class LinkDecodeError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "LinkDecodeError";
  }
}

function view(bytes: Uint8Array): DataView {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
}

function requireLength(bytes: Uint8Array, min: number, what: string): void {
  if (bytes.length < min) {
    throw new LinkDecodeError(`${what}: need ${min} bytes, got ${bytes.length}`);
  }
}

// ---------------------------------------------------------------------------
// Bulk blob (REQ_GET_ALL_PARAMS, 0xA0)

export interface BulkHeader {
  /** WireBulkParams format_version. Gate every other offset on this. */
  wireVersion: number;
  /** 0 = RP2040, 1 = RP2350. */
  platform: number;
  /** Valid channels in the payload; inputs first, then outputs. */
  numChannels: number;
  numOutputChannels: number;
  numInputChannels: number;
  maxBands: number;
  /** Total packet size including the header. */
  payloadLength: number;
  fwVersionMajor: number;
  fwVersionMinor: number;
}

/**
 * Decode the 16-byte WireHeader that opens every bulk blob. This part of the
 * layout has never moved, so it is safe to read before checking the version.
 */
export function decodeBulkHeader(blob: Uint8Array): BulkHeader {
  requireLength(blob, BULK_HEADER_SIZE, "bulk header");
  const dv = view(blob);
  return {
    wireVersion: blob[0],
    platform: blob[1],
    numChannels: blob[2],
    numOutputChannels: blob[3],
    numInputChannels: blob[4],
    maxBands: blob[5],
    payloadLength: dv.getUint16(6, true),
    fwVersionMajor: dv.getUint16(8, true),
    fwVersionMinor: dv.getUint16(10, true),
  };
}

/**
 * Master output volume in dB, float32 LE at offset 4700 (wire version 30).
 * -128 is the mute sentinel, not a real level.
 */
export function readMasterVolumeDb(blob: Uint8Array): number {
  requireLength(blob, BULK_MASTER_VOLUME_OFFSET + 4, "bulk master volume");
  return view(blob).getFloat32(BULK_MASTER_VOLUME_OFFSET, true);
}

/** Global preamp gain in dB, the first float of WireGlobalParams at offset 16. */
export function readGlobalPreampDb(blob: Uint8Array): number {
  requireLength(blob, BULK_GLOBAL_OFFSET + 4, "bulk global preamp");
  return view(blob).getFloat32(BULK_GLOBAL_OFFSET, true);
}

/** True when this blob's offsets match the layout this module was written against. */
export function isSupportedBulkVersion(header: BulkHeader): boolean {
  return header.wireVersion === BULK_WIRE_VERSION;
}

// ---------------------------------------------------------------------------
// Combined status frame (REQ_GET_STATUS 0x50, wValue 9)

export interface StatusFrame {
  /** Per-channel peak meters, u16 LE, one per channel of the bulk header. */
  peaks: number[];
  /** Core 0 load, percent. */
  cpu0: number;
  /** Core 1 load, percent. */
  cpu1: number;
  /** Sticky per-channel clip bitmask. */
  clipFlags: number;
  /** Live input channel count, absent on firmware that predates the field. */
  activeInputChannels?: number;
}

/**
 * Decode the combined meter packet.
 *
 * Layout (as captured from wire-version-30 firmware):
 *   peaks[numChannels] u16 LE, cpu0 u8, cpu1 u8, clip_flags u32 LE,
 *   active_input_channels u8
 *
 * `numChannels` is the bulk header's num_channels; the frame carries no count
 * of its own, which is why the caller must supply one. Older firmware sent
 * clip_flags as a u16 and no input count, so the tail is read by length.
 */
export function decodeStatusFrame(bytes: Uint8Array, numChannels: number): StatusFrame {
  if (!Number.isInteger(numChannels) || numChannels < 0) {
    throw new LinkDecodeError(`status frame: bad channel count ${numChannels}`);
  }
  const peaksBytes = numChannels * 2;
  requireLength(bytes, peaksBytes + 2, "status frame");
  const dv = view(bytes);
  const peaks = new Array<number>(numChannels);
  for (let i = 0; i < numChannels; i++) peaks[i] = dv.getUint16(i * 2, true);

  const cpu0 = bytes[peaksBytes];
  const cpu1 = bytes[peaksBytes + 1];
  let clipFlags = 0;
  if (bytes.length >= peaksBytes + 6) clipFlags = dv.getUint32(peaksBytes + 2, true);
  else if (bytes.length >= peaksBytes + 4) clipFlags = dv.getUint16(peaksBytes + 2, true);

  const frame: StatusFrame = { peaks, cpu0, cpu1, clipFlags };
  if (bytes.length >= peaksBytes + 7) frame.activeInputChannels = bytes[peaksBytes + 6];
  return frame;
}

/** Bytes the combined status frame occupies for a given channel count. */
export function statusFrameLength(numChannels: number): number {
  return numChannels * 2 + 7;
}

// ---------------------------------------------------------------------------
// Platform reply (REQ_GET_PLATFORM, 0x7F)

export interface PlatformInfo {
  /** 0 = RP2040, 1 = RP2350. */
  platform: number;
  /** "major.minor.patch". */
  fw: string;
  fwMajor: number;
  fwMinor: number;
  fwPatch: number;
  /** NUM_OUTPUT_CHANNELS. */
  outputs: number;
}

/**
 * Decode the platform reply: [platform, fw major, (minor << 4) | patch,
 * outputs, fw minor, fw patch].
 *
 * Bytes 4 and 5 are the full-width minor and patch and are preferred; the
 * packed nibbles in byte 2 are the legacy encoding and only stay truthful
 * while both fields are 15 or under, so they are a fallback for the 4-byte
 * short read old firmware returns.
 */
export function decodePlatform(bytes: Uint8Array): PlatformInfo {
  requireLength(bytes, 4, "platform");
  const platform = bytes[0];
  const fwMajor = bytes[1];
  const outputs = bytes[3];
  const full = bytes.length >= 6;
  const fwMinor = full ? bytes[4] : (bytes[2] >> 4) & 0x0f;
  const fwPatch = full ? bytes[5] : bytes[2] & 0x0f;
  return {
    platform,
    fw: `${fwMajor}.${fwMinor}.${fwPatch}`,
    fwMajor,
    fwMinor,
    fwPatch,
    outputs,
  };
}

// ---------------------------------------------------------------------------
// Notification packets (v2), relayed inside NOTIFY frames

/** notification_protocol_v2_spec.md section 3.3. */
export enum NotifyEvent {
  IDLE = 0x00,
  MASTER_VOLUME = 0x01,
  PARAM_CHANGED = 0x02,
  BULK_INVALIDATED = 0x03,
  PRESET_LOADED = 0x04,
  INPUT_FORMAT = 0x05,
  SIGGEN_STATE = 0x07,
  ADAT_STATE = 0x08,
  I2S_SLAVE_STATE = 0x09,
  CS_IR_LEARN = 0x0a,
  ADAT_INPUT_STATE = 0x0b,
  CS_AUX = 0x0c,
}

/** ParamSource, spec section 3.7. Unknown values mean "someone else changed it". */
export enum ParamSource {
  UNKNOWN = 0,
  HOST_SET = 1,
  BULK_SET = 2,
  PRESET = 3,
  FACTORY = 4,
  GPIO = 5,
  INTERNAL = 6,
  UAC1 = 7,
  UART = 8,
  I2C = 9,
}

interface NotifyPacketBase {
  version: number;
  event: number;
  flags: number;
  /** Monotonic 8-bit counter; a gap means the client missed events. */
  seq: number;
}

export interface ParamChangedPacket extends NotifyPacketBase {
  kind: "param_changed";
  /** offsetof into WireBulkParams; the parameter's stable identity. */
  wireOffset: number;
  wireSize: number;
  source: number;
  /** wireSize bytes, encoded exactly as in the bulk blob. */
  value: Uint8Array;
}

export interface BulkInvalidatedPacket extends NotifyPacketBase {
  kind: "bulk_invalidated";
  source: number;
}

export interface OtherNotifyPacket extends NotifyPacketBase {
  kind: "other";
  /** Bytes after the 4-byte header, undecoded. */
  body: Uint8Array;
}

export type NotifyPacket = ParamChangedPacket | BulkInvalidatedPacket | OtherNotifyPacket;

/**
 * Decode a v2 notification packet. Returns null for a v1 legacy packet (never
 * relayed over Link) or a buffer too short to hold the header, both of which a
 * client drops rather than treating as an error.
 */
export function decodeNotifyPacket(bytes: Uint8Array): NotifyPacket | null {
  if (bytes.length < 4) return null;
  const version = bytes[0];
  if (version !== 2) return null;
  const base: NotifyPacketBase = {
    version,
    event: bytes[1],
    flags: bytes[2],
    seq: bytes[3],
  };
  const dv = view(bytes);

  if (base.event === NotifyEvent.PARAM_CHANGED) {
    if (bytes.length < 12) return null;
    const wireSize = dv.getUint16(6, true);
    return {
      ...base,
      kind: "param_changed",
      wireOffset: dv.getUint16(4, true),
      wireSize,
      source: bytes[8],
      // Bytes 9..11 are reserved; the value starts at 12 and may be short-read.
      value: bytes.slice(12, Math.min(12 + wireSize, bytes.length)),
    };
  }
  if (base.event === NotifyEvent.BULK_INVALIDATED) {
    if (bytes.length < 5) return null;
    return { ...base, kind: "bulk_invalidated", source: bytes[4] };
  }
  return { ...base, kind: "other", body: bytes.slice(4) };
}

// ---------------------------------------------------------------------------
// base64, for the `_b64` fields of device.snapshot

const B64_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** Standard base64 with padding, spec 7.1. Works in the browser and in Node. */
export function bytesToBase64(bytes: Uint8Array): string {
  let out = "";
  for (let i = 0; i < bytes.length; i += 3) {
    const b0 = bytes[i];
    const b1 = i + 1 < bytes.length ? bytes[i + 1] : 0;
    const b2 = i + 2 < bytes.length ? bytes[i + 2] : 0;
    out += B64_CHARS[b0 >> 2];
    out += B64_CHARS[((b0 & 0x03) << 4) | (b1 >> 4)];
    out += i + 1 < bytes.length ? B64_CHARS[((b1 & 0x0f) << 2) | (b2 >> 6)] : "=";
    out += i + 2 < bytes.length ? B64_CHARS[b2 & 0x3f] : "=";
  }
  return out;
}

export function base64ToBytes(b64: string): Uint8Array {
  // Accept base64url too, so the same helper handles tokens.
  const clean = b64.replace(/-/g, "+").replace(/_/g, "/").replace(/[^A-Za-z0-9+/]/g, "");
  const out = new Uint8Array(Math.floor((clean.length * 3) / 4));
  let n = 0;
  let acc = 0;
  let bits = 0;
  for (const ch of clean) {
    const v = B64_CHARS.indexOf(ch);
    if (v < 0) continue;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[n++] = (acc >> bits) & 0xff;
    }
  }
  return out.subarray(0, n);
}
