import { createHash } from "node:crypto";
import { describe, expect, it } from "vitest";
import { sha256Hex } from "../src/sha256.js";

const text = (s: string) => new TextEncoder().encode(s);

describe("sha256", () => {
  it("matches the FIPS 180-4 examples", () => {
    expect(sha256Hex(text(""))).toBe(
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    );
    expect(sha256Hex(text("abc"))).toBe(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    );
    expect(sha256Hex(text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))).toBe(
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
    );
  });

  it("agrees with Node's at every length around a block boundary, and on a large image", () => {
    for (const length of [55, 56, 63, 64, 65, 119, 120, 1000]) {
      const data = Uint8Array.from({ length }, (_, i) => (i * 31 + 7) & 0xff);
      expect(sha256Hex(data)).toBe(createHash("sha256").update(data).digest("hex"));
    }
    const image = Uint8Array.from({ length: 1_000_003 }, (_, i) => (i ^ (i >> 8)) & 0xff);
    expect(sha256Hex(image)).toBe(createHash("sha256").update(image).digest("hex"));
  });
});
