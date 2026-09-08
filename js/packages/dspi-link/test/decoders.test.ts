import { describe, expect, it } from "vitest";
import {
  BULK_GLOBAL_OFFSET,
  BULK_MASTER_VOLUME_OFFSET,
  BULK_PARAMS_SIZE,
  BULK_WIRE_VERSION,
  NotifyEvent,
  ParamSource,
  base64ToBytes,
  bytesToBase64,
  decodeBulkHeader,
  decodeNotifyPacket,
  decodePlatform,
  decodeStatusFrame,
  isSupportedBulkVersion,
  readGlobalPreampDb,
  readMasterVolumeDb,
  statusFrameLength,
} from "../src/decoders.js";
import { decodeFrame } from "../src/frames.js";
import { fixtureBytes, fixtureJson } from "./fixtures.js";

interface Capture {
  bulk: {
    file: string;
    master_volume_db: number;
    num_channels: number;
    num_input_channels: number;
    num_output_channels: number;
    size: number;
    wire_version: number;
  };
  platform: { fw: string; outputs: number; platform: number; raw_hex: string };
  status: { file: string; layout: string; size: number };
  serial: string;
}

const capture = fixtureJson<Capture>("device", "capture.json");
const bulk = fixtureBytes("device", capture.bulk.file);
const status = fixtureBytes("device", capture.status.file);
const platform = fixtureBytes("device", "platform.bin");

describe("bulk blob from a real board", () => {
  it("is the size the capture recorded", () => {
    expect(bulk.length).toBe(capture.bulk.size);
    expect(bulk.length).toBe(BULK_PARAMS_SIZE);
  });

  it("decodes the header to the captured values", () => {
    const header = decodeBulkHeader(bulk);
    expect(header.wireVersion).toBe(capture.bulk.wire_version);
    expect(header.wireVersion).toBe(BULK_WIRE_VERSION);
    expect(isSupportedBulkVersion(header)).toBe(true);
    expect(header.numChannels).toBe(capture.bulk.num_channels);
    expect(header.numOutputChannels).toBe(capture.bulk.num_output_channels);
    expect(header.numInputChannels).toBe(capture.bulk.num_input_channels);
    expect(header.numChannels).toBe(header.numInputChannels + header.numOutputChannels);
    expect(header.payloadLength).toBe(bulk.length);
    expect(header.maxBands).toBe(12);
    expect(`${header.fwVersionMajor}.${header.fwVersionMinor}`).toBe("1.1");
  });

  it("reads the master volume at offset 4700", () => {
    expect(BULK_MASTER_VOLUME_OFFSET).toBe(4700);
    expect(readMasterVolumeDb(bulk)).toBe(capture.bulk.master_volume_db);
  });

  it("reads the global preamp at offset 16", () => {
    expect(BULK_GLOBAL_OFFSET).toBe(16);
    expect(readGlobalPreampDb(bulk)).toBe(0);
  });

  it("refuses a blob too short to hold the field", () => {
    expect(() => readMasterVolumeDb(bulk.subarray(0, 100))).toThrow();
    expect(() => decodeBulkHeader(new Uint8Array(4))).toThrow();
  });
});

describe("combined status frame", () => {
  const numChannels = capture.bulk.num_channels;

  it("has one u16 peak per channel of the bulk header", () => {
    const frame = decodeStatusFrame(status, numChannels);
    expect(frame.peaks.length).toBe(numChannels);
    expect(status.length).toBe(capture.status.size);
    expect(statusFrameLength(numChannels)).toBe(status.length);
  });

  it("decodes the captured meters", () => {
    const frame = decodeStatusFrame(status, numChannels);
    expect(frame.peaks.every((p) => p === 0)).toBe(true);
    expect(frame.cpu0).toBe(8);
    expect(frame.cpu1).toBe(0);
    expect(frame.clipFlags).toBe(0);
    expect(frame.activeInputChannels).toBe(2);
  });

  it("falls back to a u16 clip mask and no input count on a short frame", () => {
    const short = status.subarray(0, numChannels * 2 + 4);
    const frame = decodeStatusFrame(short, numChannels);
    expect(frame.peaks.length).toBe(numChannels);
    expect(frame.clipFlags).toBe(0);
    expect(frame.activeInputChannels).toBeUndefined();
  });

  it("reads little-endian peaks", () => {
    const bytes = new Uint8Array([0x34, 0x12, 0xff, 0x7f, 42, 3, 1, 0, 0, 0, 4]);
    const frame = decodeStatusFrame(bytes, 2);
    expect(frame.peaks).toEqual([0x1234, 0x7fff]);
    expect(frame.cpu0).toBe(42);
    expect(frame.cpu1).toBe(3);
    expect(frame.clipFlags).toBe(1);
    expect(frame.activeInputChannels).toBe(4);
  });

  it("rejects a frame too short for the channel count", () => {
    expect(() => decodeStatusFrame(new Uint8Array(4), 17)).toThrow();
  });
});

describe("platform reply", () => {
  it("decodes the captured board", () => {
    const info = decodePlatform(platform);
    expect(info.platform).toBe(capture.platform.platform);
    expect(info.fw).toBe(capture.platform.fw);
    expect(info.outputs).toBe(capture.platform.outputs);
  });

  it("decodes the platform frame fixture the same way", () => {
    const frame = decodeFrame(fixtureBytes("frames", "cmd_response_platform.bin"));
    expect(frame?.type).toBe("cmd_response");
    const info = decodePlatform((frame as { payload: Uint8Array }).payload);
    expect(info.fw).toBe("1.1.7");
    expect(info.platform).toBe(1);
    expect(info.outputs).toBe(9);
  });

  it("prefers bytes 4 and 5 over the legacy nibbles when both disagree", () => {
    // A firmware at 1.16.20 cannot be expressed in the packed nibbles at byte 2.
    const bytes = new Uint8Array([1, 1, 0x04, 9, 16, 20]);
    expect(decodePlatform(bytes).fw).toBe("1.16.20");
  });

  it("falls back to the nibbles on a 4-byte short read from old firmware", () => {
    expect(decodePlatform(new Uint8Array([0, 1, 0x16, 5])).fw).toBe("1.1.6");
  });
});

describe("notification packets", () => {
  it("decodes the PARAM_CHANGED fixture out of its NOTIFY frame", () => {
    const frame = decodeFrame(fixtureBytes("frames", "notify_param_changed.bin"));
    expect(frame?.type).toBe("notify");
    const notify = frame as { origin: number; packet: Uint8Array };
    expect(notify.origin).toBe(12);

    const packet = decodeNotifyPacket(notify.packet);
    expect(packet).not.toBeNull();
    expect(packet!.kind).toBe("param_changed");
    if (packet!.kind !== "param_changed") return;
    expect(packet!.version).toBe(2);
    expect(packet!.event).toBe(NotifyEvent.PARAM_CHANGED);
    expect(packet!.seq).toBe(41);
    expect(packet!.wireOffset).toBe(16);
    expect(packet!.wireSize).toBe(4);
    expect(packet!.source).toBe(ParamSource.HOST_SET);
    expect(packet!.value.length).toBe(4);
  });

  it("decodes BULK_INVALIDATED with its source", () => {
    const packet = decodeNotifyPacket(new Uint8Array([2, 0x03, 0, 7, ParamSource.PRESET, 0, 0, 0]));
    expect(packet?.kind).toBe("bulk_invalidated");
    if (packet?.kind !== "bulk_invalidated") return;
    expect(packet.source).toBe(ParamSource.PRESET);
    expect(packet.seq).toBe(7);
  });

  it("keeps an unmodelled event as raw bytes rather than dropping it", () => {
    const packet = decodeNotifyPacket(new Uint8Array([2, NotifyEvent.SIGGEN_STATE, 0, 9, 1, 2, 3, 0xff]));
    expect(packet?.kind).toBe("other");
    if (packet?.kind !== "other") return;
    expect(packet.event).toBe(0x07);
    expect([...packet.body]).toEqual([1, 2, 3, 0xff]);
  });

  it("returns null for a v1 legacy packet and for a runt", () => {
    expect(decodeNotifyPacket(new Uint8Array([1, 0x01, 0, 0, 0, 0, 0, 0]))).toBeNull();
    expect(decodeNotifyPacket(new Uint8Array([2, 0x02]))).toBeNull();
  });
});

describe("base64 helpers", () => {
  it("round-trips the bulk blob through the _b64 encoding", () => {
    const b64 = bytesToBase64(bulk);
    expect(b64).toBe(Buffer.from(bulk).toString("base64"));
    expect([...base64ToBytes(b64)]).toEqual([...bulk]);
  });

  it("handles every padding length", () => {
    for (const n of [0, 1, 2, 3, 4, 5]) {
      const bytes = new Uint8Array(n).map((_, i) => i * 37);
      expect([...base64ToBytes(bytesToBase64(bytes))]).toEqual([...bytes]);
    }
  });

  it("accepts base64url, which is how tokens are encoded", () => {
    const bytes = new Uint8Array([0xfb, 0xff, 0xbe]);
    const url = bytesToBase64(bytes).replace(/\+/g, "-").replace(/\//g, "_");
    expect([...base64ToBytes(url)]).toEqual([...bytes]);
  });
});
