/*
 * bridge_config.h - every tunable number in the bridge, in one place.
 *
 * Each constant carries its unit in its name and a comment giving the reason
 * for its value, with the spec section it comes from. "Link spec" means
 * spec/dspi_link_protocol_spec.md; "bridge spec" means
 * spec/esp32_bridge_spec.md. Change a value here, never at the point of use.
 *
 * This header is portable: it includes nothing from ESP-IDF, so host tests
 * can use it too.
 */

#ifndef BRIDGE_CONFIG_H
#define BRIDGE_CONFIG_H

/* ---------------------------------------------------------------------------
 * Link protocol (Link spec 3, 4, 7, 8; bridge spec 8)
 * ------------------------------------------------------------------------- */

/* The Link port: decimal of the DSPi's USB vendor id 0x2E8B (Link spec 3.1). */
#define LINK_TCP_PORT 11915

/* Port 80 serves the setup page on the setup network and a redirect to
 * LINK_TCP_PORT on the home network (bridge spec 8.8). */
#define HTTP_REDIRECT_PORT 80

/* The protocol minimum (Link spec 4); enough for a FWDATA chunk of 16 KB. */
#define LINK_MAX_FRAME_BYTES 16384

/* The protocol minimum (Link spec 7.2); the 6136-byte V32 bulk blob fits. */
#define LINK_MAX_PAYLOAD_BYTES 8192

/* Commands one session may have queued per device (bridge spec 8.1). The
 * UART runs one at a time, so a deeper queue would only add latency. */
#define LINK_MAX_INFLIGHT 4

/* Highest poll rate a session may ask for (bridge spec 8.1). */
#define LINK_POLL_MAX_HZ 20

/* Poll budget in bytes per second is the baud rate divided by this: 40,000
 * at 1 Mbaud, leaving over half the link for commands and notifications
 * (Link spec 9.2, bridge spec 7.7). */
#define LINK_POLL_BUDGET_BAUD_DIVISOR 25

/* Simultaneous sessions (bridge spec 8.2). Each session needs buffers, so
 * boards with PSRAM allow more. */
#define LINK_MAX_SESSIONS_WITHOUT_PSRAM 4
#define LINK_MAX_SESSIONS_WITH_PSRAM 8

/* Frames waiting to be sent to one session before notifications for it are
 * dropped and a RESYNC follows (bridge spec 8.2). */
#define LINK_SESSION_QUEUE_FRAMES 32

/* Keepalive (Link spec 4 and 5). */
#define LINK_PING_INTERVAL_MS 10000
#define LINK_PING_TIMEOUT_MS 30000

/* A session whose socket stays unwritable this long is closed with code
 * 1011 (Link spec 5). */
#define LINK_UNWRITABLE_TIMEOUT_MS 10000

/* A session that has not authenticated by now is closed with code 4001
 * (Link spec 5). */
#define LINK_AUTH_TIMEOUT_MS 30000

/* SETs older than this no longer claim a notification's origin (Link spec
 * 8.3, bridge spec 8.4). */
#define LINK_ORIGIN_WINDOW_MS 1000

/* An offline device keeps its handle this long before device.removed
 * (Link spec 7.4). */
#define LINK_OFFLINE_HANDLE_RETAIN_MS 60000

/* Device names remembered by serial (bridge spec 8.7). */
#define LINK_MAX_DEVICE_NAMES 8

/* ---------------------------------------------------------------------------
 * Authentication (Link spec 6.2; bridge spec 8.9)
 * ------------------------------------------------------------------------- */

#define AUTH_PIN_DIGITS 6
#define AUTH_PIN_SALT_BYTES 16

/* The PIN's hash keeps it from being read straight out of a copy of the
 * flash. With only a million possible PINs no round count stops an offline
 * search, so the rate limits below are the real defence, and the count is
 * kept low: each check runs on the hub task, and 10,000 rounds took 1.5 s on
 * an ESP32-S3, holding up every session. 1,000 take about 150 ms. */
#define AUTH_PIN_PBKDF2_ITERATIONS 1000

#define AUTH_TOKEN_BYTES 32
#define AUTH_MAX_PAIRED_CLIENTS 16

/* last_seen is saved at most this often per client, to spare flash. */
#define AUTH_LAST_SEEN_SAVE_INTERVAL_MS (60 * 60 * 1000)

/* Rate limits for a fixed PIN (Link spec 6.2). */
#define AUTH_FAILURE_WINDOW_MS 60000
#define AUTH_MAX_FAILURES_PER_ADDRESS 5
#define AUTH_MAX_FAILURES_ALL_ADDRESSES 20
#define AUTH_LOCKOUT_MS 60000

/* ---------------------------------------------------------------------------
 * DSPi UART link (bridge spec 7; firmware control_interfaces_spec.md)
 * ------------------------------------------------------------------------- */

/* UART1 exists on every supported chip; UART0 stays with the boot console
 * (bridge spec 4.4). */
#define DSPI_UART_PORT_NUMBER 1

/* The recommended DSPi setting (bridge spec 5). */
#define DSPI_UART_DEFAULT_BAUD 1000000

/* Rates tried in order when detecting the baud rate, after the saved rate
 * (bridge spec 7.5). */
#define DSPI_UART_BAUD_CANDIDATES {1000000, 921600, 460800, 230400, 115200}

/* Firmware limits on the UART (Link spec 9.2): non-bulk GET replies are cut
 * to 132 bytes and non-bulk SET payloads above 64 bytes are refused. */
#define DSPI_UART_MAX_GET_REPLY_BYTES 132
#define DSPI_UART_MAX_SET_PAYLOAD_BYTES 64

/* Pause after every response so the firmware can send queued notifications
 * (Link spec 9.2, bridge spec 7.2). */
#define DSPI_UART_IDLE_GAP_MS 2

/* Added to twice the wire time of the largest possible reply. Covers the
 * DSPi's 45 ms flash-write blackout and its 50 ms internal busy retry
 * (bridge spec 7.2). */
#define DSPI_UART_TIMEOUT_MARGIN_MS 150

/* After a timeout, discard non-notification frames until the line has been
 * silent this long (bridge spec 7.4). */
#define DSPI_UART_QUARANTINE_SILENCE_MS 20

/* Upper bound on one quarantine, so a line that never falls silent (noise,
 * a wrong baud rate) cannot hold the link forever. */
#define DSPI_UART_QUARANTINE_MAX_MS 500

/* Timeouts in a row before the UART driver is reinstalled (bridge spec 7.4). */
#define DSPI_UART_TIMEOUTS_BEFORE_REINSTALL 3

/* Failed requests in a row before the device is reported lost. One timeout
 * never is (Link spec 8.2, bridge spec 7.6). */
#define DSPI_UART_FAILURES_BEFORE_LOST 3

/* How often to look for a device while none is known (bridge spec 7.6). */
#define DSPI_UART_PROBE_INTERVAL_MS 2000

/* Idle time after which a heartbeat request checks the device is still
 * there (bridge spec 7.6). */
#define DSPI_UART_HEARTBEAT_IDLE_MS 1000

/* While no request is waiting, the uart task checks for notifications this
 * often. Requests themselves start at once; this only bounds how late a
 * notification can be read. */
#define DSPI_UART_LISTEN_SLICE_MS 5

/* The ESP-IDF UART driver's receive ring. Holds over 40 ms of traffic at
 * 1 Mbaud, longer than the uart task ever goes without reading. */
#define DSPI_UART_DRIVER_RX_BUFFER_BYTES 4096

/* Requests waiting for the uart task: room for every session's in-flight
 * commands (8 sessions x LINK_MAX_INFLIGHT) plus the hub's own polls. */
#define DSPI_UART_QUEUE_DEPTH 40

/* ---------------------------------------------------------------------------
 * Tasks (bridge spec 6.2)
 * ------------------------------------------------------------------------- */

/* The uart task needs little stack: its buffers are static. */
#define DSPI_UART_TASK_STACK_BYTES 4096

/* Above the HTTP server and the hub, so a request in progress on the wire
 * is never held up by network work. */
#define DSPI_UART_TASK_PRIORITY 10

/* ---------------------------------------------------------------------------
 * Networking (bridge spec 9)
 * ------------------------------------------------------------------------- */

/* A saved home network unreachable this long brings back the setup network
 * (bridge spec 9.1). */
#define WIFI_FALLBACK_AFTER_MS (3 * 60 * 1000)

/* Station retry delays while connecting; the last repeats (bridge spec 9.1). */
#define WIFI_RETRY_DELAYS_MS {5000, 10000, 20000, 30000}

/* hub.wifi.join gives the new network this long before returning to the old
 * one (Link spec 7.11). */
#define WIFI_JOIN_TIMEOUT_MS 30000

/* The setup network stays up after the home network connects until no phone
 * has been on it for this long (bridge spec 9.1). */
#define SETUP_NETWORK_IDLE_OFF_MS 60000

/* Delay between POST /setup/v1/finish and the setup network going off, so
 * the reply reaches the phone (bridge spec 9.3). */
#define SETUP_NETWORK_FINISH_DELAY_MS 5000

/* The setup network goes off by itself this long after a successful join
 * if the page never sends finish (bridge spec 9.3). */
#define SETUP_NETWORK_AUTO_OFF_MS (2 * 60 * 1000)

/* Phones or computers on the setup network at once (bridge spec 9.2). */
#define SETUP_NETWORK_MAX_STATIONS 4

/* ---------------------------------------------------------------------------
 * BOOT button (bridge spec 12.1)
 * ------------------------------------------------------------------------- */

#define BUTTON_FORGET_WIFI_HOLD_MS 5000
#define BUTTON_FACTORY_RESET_HOLD_MS 15000
#define BUTTON_CANCEL_HOLD_MS 30000

/* ---------------------------------------------------------------------------
 * Firmware updates (bridge spec 14)
 * ------------------------------------------------------------------------- */

/* A new image confirms itself after running this long with every task up;
 * a crash before then rolls back (bridge spec 14.2). */
#define OTA_CONFIRM_AFTER_MS 60000

#endif /* BRIDGE_CONFIG_H */
