/*
 * Standard base64 with padding, for the `_b64` fields of control messages
 * (Link spec 7.1), and base64url for pairing tokens (Link spec 6.2).
 *
 * Browsers have atob/btoa and Node has Buffer, but neither works in both and
 * atob deals in strings rather than bytes, so this small version is used
 * everywhere instead.
 */

const BASE64_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/**
 * Encodes bytes as standard base64 with padding.
 *
 * @param bytes The bytes to encode.
 * @returns The base64 text.
 */
export function bytesToBase64(bytes: Uint8Array): string {
  let text = "";
  for (let index = 0; index < bytes.length; index += 3) {
    const first = bytes[index];
    const hasSecond = index + 1 < bytes.length;
    const hasThird = index + 2 < bytes.length;
    const second = hasSecond ? bytes[index + 1] : 0;
    const third = hasThird ? bytes[index + 2] : 0;
    text += BASE64_ALPHABET[first >> 2];
    text += BASE64_ALPHABET[((first & 0x03) << 4) | (second >> 4)];
    text += hasSecond ? BASE64_ALPHABET[((second & 0x0f) << 2) | (third >> 6)] : "=";
    text += hasThird ? BASE64_ALPHABET[third & 0x3f] : "=";
  }
  return text;
}

/**
 * Decodes base64 or base64url text. Padding, whitespace and any other
 * character outside the alphabet are skipped, so the same helper handles a
 * snapshot blob and a pairing token.
 *
 * @param text The encoded text.
 * @returns The decoded bytes.
 */
export function base64ToBytes(text: string): Uint8Array {
  const clean = text
    .replace(/-/g, "+")
    .replace(/_/g, "/")
    .replace(/[^A-Za-z0-9+/]/g, "");
  const bytes = new Uint8Array(Math.floor((clean.length * 3) / 4));
  let length = 0;
  let accumulator = 0;
  let bits = 0;
  for (const character of clean) {
    accumulator = (accumulator << 6) | BASE64_ALPHABET.indexOf(character);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes[length++] = (accumulator >> bits) & 0xff;
    }
  }
  return bytes.subarray(0, length);
}
