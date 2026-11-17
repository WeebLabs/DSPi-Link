/*
 * dspi_commands.h - the firmware vendor commands the bridge itself names.
 *
 * Firmware 1.1.7 addresses each command by a page and a command number. The
 * 16-bit command ID is (page << 8) | command: the page travels in the high
 * byte of wIndex and the command number in bRequest (firmware
 * `Documentation/protocol/dspi_commands.json`, Link spec 8.2). Clients'
 * commands pass through the bridge untouched; only the commands the bridge
 * sends, or treats specially, are listed here.
 */

#ifndef DSPI_COMMANDS_H
#define DSPI_COMMANDS_H

#include <stdint.h>

/* bRequest and wIndex for command `id`. The bridge sends its own requests
 * with wIndex low byte 0, which the UART accepts. */
#define DSPI_CMD_REQUEST(id) ((uint8_t)((id) & 0xFF))
#define DSPI_CMD_INDEX(id) ((uint16_t)((id) & 0xFF00))

/* The command ID a request names. */
#define DSPI_CMD_ID(request, index) ((uint16_t)(((index) & 0xFF00) | (request)))

/* SYS page */
#define DSPI_CMD_SYS_PROTOCOL_INFO 0x0110 /* GET, 16 bytes: "DSPi", major, minor, ... */
#define DSPI_CMD_SYS_PLATFORM 0x0120      /* GET, 7 bytes, the 1.1.6 layout */
#define DSPI_CMD_SYS_SERIAL 0x0121        /* GET, 16 bytes */
#define DSPI_CMD_SYS_DEVICE_NAME 0x0124   /* GET 32 bytes, or SET 1 to 32 */

/* STORE page */
#define DSPI_CMD_STORE_ALL_PARAMS 0x0240 /* GET or SET the whole bulk blob */

/* ROUTE and XPORT pages */
#define DSPI_CMD_XPORT_UART_CONFIG 0x0B10 /* GET 8 bytes; the SET is USB only */
#define DSPI_CMD_XPORT_STATUS 0x0B12      /* GET, 8 bytes */

/* MEAS page: the analyser commands a hub arbitrates (Link spec 7.15). The
 * three reads are the registry's KEEPALIVE GETs: reading keeps it running. */
#define DSPI_CMD_MEAS_LEVELS 0x0910 /* GET, 2 x channels + 7 bytes: the meter frame */
#define DSPI_CMD_MEAS_RTA_BANDS 0x0923
#define DSPI_CMD_MEAS_RTA_BINS 0x0924
#define DSPI_CMD_MEAS_RTA_BANDS_ALL 0x0926
#define DSPI_CMD_MEAS_RTA_START 0x0928 /* Zero-length SET */
#define DSPI_CMD_MEAS_RTA_STOP 0x0929  /* Zero-length SET */

/* SYS.PROTOCOL_INFO's reply: the magic, then the protocol major and minor. */
#define DSPI_PROTOCOL_INFO_BYTES 16
#define DSPI_PROTOCOL_MAJOR 1 /* The only major this bridge speaks */

/* Firmware 1.1.6 and older: the only request the bridge sends such a
 * device, to show which version it runs (bridge spec 7.6). */
#define DSPI_LEGACY_REQ_GET_PLATFORM 0x7F

#endif /* DSPI_COMMANDS_H */
