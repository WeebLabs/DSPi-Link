/* The binary frame codec (Link spec 8) against fixtures/frames and hand-made frames. */

import { describe, expect, it } from "vitest";
import {
  FrameType,
  LinkFrameError,
  LinkStatus,
  ResyncReason,
  decodeFrame,
  encodeFrame,
  statusName,
} from "../src/frames.js";
import { fixtureBytes, fixtureJson, fixtureNames, toHex } from "./fixtures.js";

interface FrameFixture {
  file: string;
  type: string;
  hex: string;
  tag?: number;
  handle?: number;
  slot?: number;
  status?: number;
  origin?: number;
  reason?: number;
  offset?: number;
  direction?: string;
  bRequest?: number;
  wValue?: number;
  wIndex?: number;
  wLength?: number;
  payload_hex?: string;
  data_hex?: string;
  payload_length?: number;
  packet?: Record<string, number>;
}

const catalogue = fixtureJson<{ frames: FrameFixture[] }>("frames", "frames.json");

describe("frames.json fixtures", () => {
  it("covers every .bin in fixtures/frames", () => {
    const onDisk = fixtureNames("frames", ".bin");
    expect(catalogue.frames.map((f) => f.file).sort()).toEqual(onDisk);
  });

  for (const fx of catalogue.frames) {
    describe(fx.file, () => {
      const bytes = fixtureBytes("frames", fx.file);

      it("matches the hex in the catalogue", () => {
        expect(toHex(bytes)).toBe(fx.hex);
      });

      it("decodes to the documented values", () => {
        const frame = decodeFrame(bytes);
        expect(frame).not.toBeNull();
        if (!frame) return;
        expect(frame.type).toBe(fx.type);

        if (fx.tag !== undefined) expect(frame.tag).toBe(fx.tag);
        if (fx.handle !== undefined) {
          expect((frame as { handle: number }).handle).toBe(fx.handle);
        }

        switch (frame.type) {
          case "cmd_request":
            expect(frame.direction).toBe(fx.direction);
            expect(frame.bRequest).toBe(fx.bRequest);
            expect(frame.wValue).toBe(fx.wValue);
            expect(frame.wIndex).toBe(fx.wIndex);
            expect(frame.wLength).toBe(fx.wLength);
            if (fx.payload_hex) expect(toHex(frame.payload!)).toBe(fx.payload_hex);
            else expect(frame.payload).toBeUndefined();
            break;
          case "cmd_response":
            expect(frame.status).toBe(fx.status);
            expect(toHex(frame.payload)).toBe(fx.payload_hex ?? "");
            break;
          case "notify":
            expect(frame.origin).toBe(fx.origin);
            expect(frame.packet.length).toBeGreaterThan(0);
            break;
          case "poll":
            expect(frame.slot).toBe(fx.slot);
            expect(frame.payload.length).toBe(fx.payload_length);
            break;
          case "fwdata":
            expect(frame.offset).toBe(fx.offset);
            expect(toHex(frame.data)).toBe(fx.data_hex);
            break;
          case "fwdata_ack":
            expect(frame.offset).toBe(fx.offset);
            expect(frame.status).toBe(fx.status);
            break;
          case "resync":
            expect(frame.reason).toBe(fx.reason);
            break;
        }
      });

      it("re-encodes byte-identically", () => {
        const frame = decodeFrame(bytes)!;
        expect(toHex(encodeFrame(frame))).toBe(fx.hex);
      });
    });
  }
});

describe("frame codec edge cases", () => {
  it("returns null for an unknown frame type so a receiver can drop it", () => {
    expect(decodeFrame(new Uint8Array([0x7e, 0, 0, 0, 1, 2, 3]))).toBeNull();
    expect(decodeFrame(new Uint8Array([0xf0, 0, 0, 0]))).toBeNull();
  });

  it("throws LinkFrameError on a truncated frame of a known type", () => {
    expect(() => decodeFrame(new Uint8Array([FrameType.CMD_RESPONSE, 0, 1, 0]))).toThrow(
      LinkFrameError,
    );
    expect(() => decodeFrame(new Uint8Array([0x01, 0]))).toThrow(LinkFrameError);
  });

  it("encodes a SET whose wLength follows the payload", () => {
    const out = encodeFrame({
      type: "cmd_request",
      tag: 7,
      handle: 0,
      direction: "set",
      bRequest: 0xd2,
      wValue: 0,
      wIndex: 2,
      payload: new Uint8Array([0x00, 0x00, 0xa0, 0xc1]),
    });
    expect(toHex(out)).toBe("010007000000d2000000020004000000a0c1");
  });

  it("writes a zero tag for RESYNC, whose tag is unused", () => {
    expect(
      toHex(encodeFrame({ type: "resync", handle: 3, reason: ResyncReason.CACHE_REBUILT })),
    ).toBe("050000000302");
  });

  it("names the status codes of spec 8.1", () => {
    expect(statusName(LinkStatus.OK)).toBe("OK");
    expect(statusName(LinkStatus.DENIED)).toBe("DENIED");
    expect(statusName(0x42)).toBe("0x42");
  });
});
