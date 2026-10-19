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
const IMAGE_HEADER_BYTES = 24;
const SEGMENT_HEADER_BYTES = 8;
/** esp_image_header_t.chip_id, a u16. */
const CHIP_ID_OFFSET = 12;
const APP_DESC_OFFSET = IMAGE_HEADER_BYTES + SEGMENT_HEADER_BYTES;
const APP_DESC_MAGIC = 0xabcd5432;
/** esp_app_desc_t: magic, secure_version and reserved words, then version[32] and project_name[32]. */
const APP_VERSION_OFFSET = 16;
const APP_PROJECT_OFFSET = 48;
const APP_STRING_BYTES = 32;
/** Enough of esp_app_desc_t to hold everything read from it. */
const APP_DESC_READ_BYTES = APP_PROJECT_OFFSET + APP_STRING_BYTES;

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

/**
 * The chip's name as people write it: "esp32s3" becomes "ESP32-S3".
 *
 * @param chip A chip name as hub.stats gives it.
 * @returns The name for the screen.
 */
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

/**
 * Reads a firmware file's own description.
 *
 * @param bytes The whole file.
 * @returns What the file says about itself, or null if it is not an ESP32 app image.
 */
export function readFirmwareInfo(bytes: Uint8Array): FirmwareInfo | null {
  if (bytes.length < APP_DESC_OFFSET + APP_DESC_READ_BYTES || bytes[0] !== IMAGE_MAGIC) {
    return null;
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint32(APP_DESC_OFFSET, true) !== APP_DESC_MAGIC) return null;
  const chipId = view.getUint16(CHIP_ID_OFFSET, true);
  return {
    chip: CHIP_IDS[chipId] ?? `chip ${chipId}`,
    version: cString(bytes, APP_DESC_OFFSET + APP_VERSION_OFFSET, APP_STRING_BYTES),
    project: cString(bytes, APP_DESC_OFFSET + APP_PROJECT_OFFSET, APP_STRING_BYTES),
  };
}
