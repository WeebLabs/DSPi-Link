/*
 * Firmware vendor commands by their 16-bit command ID, and how a CMD frame
 * carries one (Link spec 8.2).
 *
 * DSPi firmware 1.1.7 addresses each command by a page and a command number.
 * The command ID is (page << 8) | command: the command number travels in
 * bRequest and the page in the high byte of wIndex, whose low byte names the
 * vendor interface, 2. The full catalogue is the firmware's registry,
 * copied into policy/firmware/dspi_commands.json; the IDs below are the ones
 * this library and its apps send.
 */

/** Command IDs, `(page << 8) | command`. */
export const Command = {
  /** GET, 16 bytes: "DSPi", protocol major and minor, limits. Send it first. */
  SYS_PROTOCOL_INFO: 0x0110,
  /** GET: every implemented page and its revision. */
  SYS_PAGE_LIST: 0x0111,
  /** GET, 64 bytes, wValue = page: GET and SET bitmaps of the page's commands. */
  SYS_PAGE_COMMANDS: 0x0112,
  /** GET, 8 bytes, wValue = (page << 8) | command: one command's flags and sizes. */
  SYS_COMMAND_INFO: 0x0113,
  /** GET, 8 bytes: why the last USB request was refused (Link spec 9.1). */
  SYS_LAST_RESULT: 0x0114,
  /** GET, 7 bytes, the 1.1.6 platform layout. */
  SYS_PLATFORM: 0x0120,
  /** GET, 16 bytes. */
  SYS_SERIAL: 0x0121,
  /** GET 32 bytes, or SET 1 to 32 bytes. */
  SYS_DEVICE_NAME: 0x0124,
  /** Zero-length SET; admin only. */
  SYS_ENTER_BOOTLOADER: 0x0130,
  /** GET or SET of the whole bulk blob (6136 bytes for wire format 32). */
  STORE_ALL_PARAMS: 0x0240,
  /** GET or SET, a little-endian f32 in dB. */
  ROUTE_MASTER_VOLUME: 0x0550,
  /** GET, 2 x channels + 7 bytes: the peak meter frame. */
  MEAS_LEVELS: 0x0910,
  MEAS_RTA_CONFIG: 0x0920,
  MEAS_RTA_BANDS: 0x0923,
  MEAS_RTA_BINS: 0x0924,
  /** USB only: through a bridge it answers 0x16 VC_ERR_BLOCKED. */
  MEAS_RTA_BANDS_ALL: 0x0926,
  /** Zero-length SETs. */
  MEAS_RTA_START: 0x0928,
  MEAS_RTA_STOP: 0x0929,
  MEAS_RTA_RESET: 0x092a,
} as const;

/** The vendor interface number, the low byte of every client's wIndex. */
export const VENDOR_INTERFACE = 2;

/**
 * How a CMD frame carries command `id` (Link spec 8.2).
 *
 * @param id A command ID, `(page << 8) | command`.
 * @returns Its bRequest and wIndex.
 */
export function commandAddress(id: number): { bRequest: number; wIndex: number } {
  if (!Number.isInteger(id) || id < 0x0100 || id > 0xffff) {
    throw new RangeError(`0x${id.toString(16)} is not a paged command ID`);
  }
  return { bRequest: id & 0xff, wIndex: (id & 0xff00) | VENDOR_INTERFACE };
}

/**
 * The command ID a CMD frame or poll names.
 *
 * @param bRequest The command number.
 * @param wIndex The page in its high byte.
 * @returns `(page << 8) | command`.
 */
export function commandId(bRequest: number, wIndex: number): number {
  return (wIndex & 0xff00) | (bRequest & 0xff);
}
