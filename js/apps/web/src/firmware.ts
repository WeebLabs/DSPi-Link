/*
 * Reads what an ESP32 firmware file says about itself, so the settings page
 * can refuse a wrong file before uploading it and say which version it is
 * about to install. The bridge checks the same things again (bridge spec
 * 14.2); this is only to give a clear answer sooner.
 *
 * The layout is ESP-IDF's app image: a 24-byte image header, an 8-byte
 * header for the first segment, then the app description (esp_app_desc_t).
 */

export interface FirmwareInfo {
  /** As hub.stats names chips: "esp32", "esp32s3", ... or "chip 23". */
  chip: string;
  version: string;
  /** "dspi_bridge" for bridge firmware. */
  project: string;
}

const IMAGE_MAGIC = 0xe9;
const APP_DESC_OFFSET = 24 + 8;
const APP_DESC_MAGIC = 0xabcd5432;

/** ESP-IDF's esp_chip_id_t values. */
const CHIP_IDS: Record<number, string> = {
  0: "esp32",
  2: "esp32s2",
  5: "esp32c3",
  9: "esp32s3",
  12: "esp32c2",
  13: "esp32c6",
  16: "esp32h2",
  18: "esp32p4",
};

/** The chip's name as people write it: "esp32s3" becomes "ESP32-S3". */
export function chipLabel(chip: string): string {
  const match = /^esp32(\w*)$/.exec(chip);
  if (!match) return chip;
  return match[1] ? `ESP32-${match[1].toUpperCase()}` : "ESP32";
}

/** A NUL-terminated string of at most `length` bytes at `offset`. */
function cString(bytes: Uint8Array, offset: number, length: number): string {
  const field = bytes.subarray(offset, offset + length);
  const end = field.indexOf(0);
  return new TextDecoder().decode(end >= 0 ? field.subarray(0, end) : field);
}

/** What the file says about itself, or null if it is not an ESP32 app image. */
export function readFirmwareInfo(bytes: Uint8Array): FirmwareInfo | null {
  if (bytes.length < APP_DESC_OFFSET + 112 || bytes[0] !== IMAGE_MAGIC) return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint32(APP_DESC_OFFSET, true) !== APP_DESC_MAGIC) return null;
  const chipId = view.getUint16(12, true);
  return {
    chip: CHIP_IDS[chipId] ?? `chip ${chipId}`,
    version: cString(bytes, APP_DESC_OFFSET + 16, 32),
    project: cString(bytes, APP_DESC_OFFSET + 48, 32),
  };
}
