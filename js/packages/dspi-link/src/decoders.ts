/*
 * Decoders for the firmware payloads Link carries verbatim.
 *
 * Link itself never interprets these bytes (Link spec 1): a client decodes
 * them with the same code it would use over USB. This module covers only what
 * a simple client needs, deliberately small: the bulk header, two bulk
 * accessors, the combined status frame, the platform reply and the v2
 * notification packet. The whole WireBulkParams struct is out of scope.
 *
 * Every offset below is checked against a range of wire-format versions and
 * is only valid while the header's format_version falls inside it. Check
 * with isSupportedBulkVersion before reading.
 */

/** Oldest wire format the bulk offsets below were checked against. */
export const BULK_MIN_WIRE_VERSION = 30;

/** Newest wire format checked (firmware bulk_params.h). Raise only after re-checking every offset. */
export const BULK_WIRE_VERSION = 32;

/** Byte length of WireBulkParams at BULK_WIRE_VERSION. Other versions differ; use the header's payloadLength. */
export const BULK_PARAMS_BYTES = 6136;

/** @deprecated Use BULK_PARAMS_BYTES. */
export const BULK_PARAMS_SIZE = BULK_PARAMS_BYTES;

/** WireHeader, 16 bytes at offset 0. Stable since the wire format began. */
export const BULK_HEADER_BYTES = 16;

/** @deprecated Use BULK_HEADER_BYTES. */
export const BULK_HEADER_SIZE = BULK_HEADER_BYTES;

/** WireGlobalParams; its first float is preamp_gain_db. Offset 16 since V16. */
export const BULK_GLOBAL_OFFSET = 16;

/** WireMasterVolume.master_volume_db, float32 LE. Offset 4700 from V30 to V32 (V31 and V32 only append). */
export const BULK_MASTER_VOLUME_OFFSET = 4700;

/** master_volume_db sentinel meaning muted (the firmware's mute value). */
export const MASTER_VOLUME_MUTE_DB = -128;

/** The `platform` byte of the bulk header and REQ_GET_PLATFORM for an RP2040. */
export const WIRE_PLATFORM_RP2040 = 0;

/** The `platform` byte of the bulk header and REQ_GET_PLATFORM for an RP2350. */
export const WIRE_PLATFORM_RP2350 = 1;

const FLOAT32_BYTES = 4;

/** Thrown when a payload is too short for what is being read from it. */
export class LinkDecodeError extends Error {
  /** @param message What could not be decoded, and why. */
  constructor(message: string) {
    super(message);
    this.name = "LinkDecodeError";
  }
}

function dataViewOf(bytes: Uint8Array): DataView {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
}

function requireLength(bytes: Uint8Array, minimum: number, what: string): void {
  if (bytes.length < minimum) {
    throw new LinkDecodeError(`${what}: need ${minimum} bytes, got ${bytes.length}`);
  }
}

// ---------------------------------------------------------------------------
// Bulk blob (REQ_GET_ALL_PARAMS, 0xA0)

/** The 16-byte WireHeader at the start of every bulk blob. */
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
 * Decodes the 16-byte WireHeader that opens every bulk blob. This part of the
 * layout has never moved, so it is safe to read before checking the version.
 *
 * @param blob A REQ_GET_ALL_PARAMS response or a snapshot's `bulk`.
 * @returns The header fields.
 * @throws LinkDecodeError when the blob is shorter than the header.
 */
export function decodeBulkHeader(blob: Uint8Array): BulkHeader {
  requireLength(blob, BULK_HEADER_BYTES, "bulk header");
  const view = dataViewOf(blob);
  return {
    wireVersion: blob[0],
    platform: blob[1],
    numChannels: blob[2],
    numOutputChannels: blob[3],
    numInputChannels: blob[4],
    maxBands: blob[5],
    payloadLength: view.getUint16(6, true),
    fwVersionMajor: view.getUint16(8, true),
    fwVersionMinor: view.getUint16(10, true),
  };
}

/**
 * Reads the master output volume, float32 LE at offset 4700 (wire versions 30
 * to 32). -128 is the mute sentinel, not a real level.
 *
 * @param blob A bulk blob whose version isSupportedBulkVersion accepts.
 * @returns The level in dB.
 * @throws LinkDecodeError when the blob is too short.
 */
export function readMasterVolumeDb(blob: Uint8Array): number {
  requireLength(blob, BULK_MASTER_VOLUME_OFFSET + FLOAT32_BYTES, "bulk master volume");
  return dataViewOf(blob).getFloat32(BULK_MASTER_VOLUME_OFFSET, true);
}

/**
 * Reads the global preamp gain, the first float of WireGlobalParams at offset 16.
 *
 * @param blob A bulk blob whose version isSupportedBulkVersion accepts.
 * @returns The gain in dB.
 * @throws LinkDecodeError when the blob is too short.
 */
export function readGlobalPreampDb(blob: Uint8Array): number {
  requireLength(blob, BULK_GLOBAL_OFFSET + FLOAT32_BYTES, "bulk global preamp");
  return dataViewOf(blob).getFloat32(BULK_GLOBAL_OFFSET, true);
}

/**
 * Whether the blob's wire version is one these offsets were checked against.
 * Later versions are refused, not assumed: a version bump can move fields.
 *
 * @param header The blob's decoded header.
 * @returns True when the bulk accessors above may be used.
 */
export function isSupportedBulkVersion(header: BulkHeader): boolean {
  return header.wireVersion >= BULK_MIN_WIRE_VERSION && header.wireVersion <= BULK_WIRE_VERSION;
}

// ---------------------------------------------------------------------------
// Combined status frame (REQ_GET_STATUS 0x50, wValue 9)

/** The combined meter packet, decoded. */
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

/** Bytes after the peaks: cpu0, cpu1, clip_flags (u32) and the input count. */
const STATUS_TAIL_BYTES = 7;

/**
 * Decodes the combined meter packet.
 *
 * Layout (as captured from v1.1.6-beta5 firmware, wire version 32):
 *   peaks[numChannels] u16 LE, cpu0 u8, cpu1 u8, clip_flags u32 LE,
 *   active_input_channels u8
 *
 * The frame carries no channel count of its own, which is why the caller
 * must supply the bulk header's. Older firmware sent clip_flags as a u16 and
 * no input count, so the tail is read by length.
 *
 * @param bytes The REQ_GET_STATUS wValue 9 response or a POLL payload.
 * @param numChannels The bulk header's num_channels.
 * @returns The decoded frame.
 * @throws LinkDecodeError on a bad channel count or a frame too short for it.
 */
export function decodeStatusFrame(bytes: Uint8Array, numChannels: number): StatusFrame {
  if (!Number.isInteger(numChannels) || numChannels < 0) {
    throw new LinkDecodeError(`status frame: bad channel count ${numChannels}`);
  }
  const peaksBytes = numChannels * 2;
  requireLength(bytes, peaksBytes + 2, "status frame");
  const view = dataViewOf(bytes);
  const peaks = new Array<number>(numChannels);
  for (let channel = 0; channel < numChannels; channel++) {
    peaks[channel] = view.getUint16(channel * 2, true);
  }

  const cpu0 = bytes[peaksBytes];
  const cpu1 = bytes[peaksBytes + 1];
  let clipFlags = 0;
  if (bytes.length >= peaksBytes + 6) clipFlags = view.getUint32(peaksBytes + 2, true);
  else if (bytes.length >= peaksBytes + 4) clipFlags = view.getUint16(peaksBytes + 2, true);

  const frame: StatusFrame = { peaks, cpu0, cpu1, clipFlags };
  if (bytes.length >= peaksBytes + STATUS_TAIL_BYTES) {
    frame.activeInputChannels = bytes[peaksBytes + 6];
  }
  return frame;
}

/**
 * The length of the combined status frame, for a poll's `len` (Link spec 7.6).
 *
 * @param numChannels The bulk header's num_channels.
 * @returns The frame's length in bytes.
 */
export function statusFrameLength(numChannels: number): number {
  return numChannels * 2 + STATUS_TAIL_BYTES;
}

// ---------------------------------------------------------------------------
// Platform reply (REQ_GET_PLATFORM, 0x7F)

/** The platform reply, decoded. */
export interface PlatformInfo {
  /** 0 = RP2040, 1 = RP2350. */
  platform: number;
  /** "major.minor.patch", without the beta number. */
  fw: string;
  fwMajor: number;
  fwMinor: number;
  fwPatch: number;
  /** Beta number, 0 for a final release; undefined on a reply shorter than 7 bytes. */
  fwBeta?: number;
  /** NUM_OUTPUT_CHANNELS. */
  outputs: number;
}

/** The short read old firmware returns: platform, major, packed minor and patch, outputs. */
const PLATFORM_SHORT_BYTES = 4;
/** With the full-width minor and patch in bytes 4 and 5. */
const PLATFORM_WIDE_BYTES = 6;
/** With the beta number in byte 6. */
const PLATFORM_FULL_BYTES = 7;

/**
 * Decodes the platform reply: [platform, fw major, (minor << 4) | patch,
 * outputs, fw minor, fw patch, fw beta]. Request 7 bytes; the device clamps
 * the reply to wLength, and betas of one patch differ only in byte 6.
 *
 * Bytes 4 and 5 are the full-width minor and patch and are preferred; the
 * packed nibbles in byte 2 are the legacy encoding and only stay truthful
 * while both fields are 15 or under, so they are a fallback for the 4-byte
 * short read old firmware returns.
 *
 * @param bytes The REQ_GET_PLATFORM response.
 * @returns The decoded reply.
 * @throws LinkDecodeError when the reply is shorter than 4 bytes.
 */
export function decodePlatform(bytes: Uint8Array): PlatformInfo {
  requireLength(bytes, PLATFORM_SHORT_BYTES, "platform");
  const platform = bytes[0];
  const fwMajor = bytes[1];
  const outputs = bytes[3];
  const wide = bytes.length >= PLATFORM_WIDE_BYTES;
  const fwMinor = wide ? bytes[4] : (bytes[2] >> 4) & 0x0f;
  const fwPatch = wide ? bytes[5] : bytes[2] & 0x0f;
  const info: PlatformInfo = {
    platform,
    fw: `${fwMajor}.${fwMinor}.${fwPatch}`,
    fwMajor,
    fwMinor,
    fwPatch,
    outputs,
  };
  if (bytes.length >= PLATFORM_FULL_BYTES) info.fwBeta = bytes[6];
  return info;
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

/** A PARAM_CHANGED packet: one parameter's new bytes. */
export interface ParamChangedPacket extends NotifyPacketBase {
  kind: "param_changed";
  /** offsetof into WireBulkParams; the parameter's stable identity. */
  wireOffset: number;
  wireSize: number;
  source: number;
  /** wireSize bytes, encoded exactly as in the bulk blob. */
  value: Uint8Array;
}

/** A BULK_INVALIDATED packet: re-read the whole blob. */
export interface BulkInvalidatedPacket extends NotifyPacketBase {
  kind: "bulk_invalidated";
  source: number;
}

/** Any other event, left undecoded. */
export interface OtherNotifyPacket extends NotifyPacketBase {
  kind: "other";
  /** Bytes after the 4-byte header, undecoded. */
  body: Uint8Array;
}

/** A decoded v2 notification packet. */
export type NotifyPacket = ParamChangedPacket | BulkInvalidatedPacket | OtherNotifyPacket;

const NOTIFY_VERSION = 2;
const NOTIFY_HEADER_BYTES = 4;
/** PARAM_CHANGED: header, offset u16, size u16, source u8, three reserved bytes. */
const PARAM_CHANGED_VALUE_OFFSET = 12;
/** BULK_INVALIDATED: header and source u8. */
const BULK_INVALIDATED_BYTES = 5;

/**
 * Decodes a v2 notification packet. A v1 legacy packet (never relayed over
 * Link) and a buffer too short for its event come back as null, which a client
 * drops rather than treating as an error.
 *
 * @param bytes A NOTIFY frame's `packet`.
 * @returns The decoded packet, or null.
 */
export function decodeNotifyPacket(bytes: Uint8Array): NotifyPacket | null {
  if (bytes.length < NOTIFY_HEADER_BYTES || bytes[0] !== NOTIFY_VERSION) return null;
  const base: NotifyPacketBase = {
    version: bytes[0],
    event: bytes[1],
    flags: bytes[2],
    seq: bytes[3],
  };
  const view = dataViewOf(bytes);

  if (base.event === NotifyEvent.PARAM_CHANGED) {
    if (bytes.length < PARAM_CHANGED_VALUE_OFFSET) return null;
    const wireSize = view.getUint16(6, true);
    const valueEnd = Math.min(PARAM_CHANGED_VALUE_OFFSET + wireSize, bytes.length);
    return {
      ...base,
      kind: "param_changed",
      wireOffset: view.getUint16(4, true),
      wireSize,
      source: bytes[8],
      // The value may be a short read; the caller checks its length.
      value: bytes.slice(PARAM_CHANGED_VALUE_OFFSET, valueEnd),
    };
  }
  if (base.event === NotifyEvent.BULK_INVALIDATED) {
    if (bytes.length < BULK_INVALIDATED_BYTES) return null;
    return { ...base, kind: "bulk_invalidated", source: bytes[4] };
  }
  return { ...base, kind: "other", body: bytes.slice(NOTIFY_HEADER_BYTES) };
}
