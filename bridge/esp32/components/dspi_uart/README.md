# dspi_uart

The link to the DSPi over UART (bridge spec 7) and the uart task that owns it
(bridge spec 6.2).

| File | What it does | Portable |
|------|--------------|----------|
| `include/dspi_link.h`, `dspi_link.c` | The link engine: one request at a time with the idle gap, timeouts sized to the baud rate, retries, quarantine after a request with no valid answer, a UART restart after repeated ones, the heartbeat, baud detection and reading the device's identity. Reaches the UART and clock only through `dspi_port_t`. | yes |
| `include/dspi_uart.h`, `dspi_uart.c` | The uart task: supplies the real ESP32 UART as a `dspi_port_t`, finds the DSPi, runs queued requests, relays notifications, sends heartbeats, moves the link to new pins (keeping the old ones if the new fail), and reports the device found or lost. | no |

Public API:

- The uart task: `dspi_uart_start()`, `dspi_uart_submit()`,
  `dspi_uart_reconfigure()`, `dspi_uart_get_status()` and
  `dspi_uart_is_alive()`. Results arrive at the handlers in
  `dspi_uart_config_t`, which run on the uart task and must copy what they
  keep.
- The link engine, used only by the uart task and the tests:
  `dspi_link_init()`, `dspi_link_transact()`, `dspi_link_listen()`,
  `dspi_link_heartbeat()`, `dspi_link_find_baud()`,
  `dspi_link_read_identity()`, `dspi_link_set_baud()`, `dspi_link_baud()`,
  `dspi_link_idle_ms()`, `dspi_link_failures_in_a_row()` and
  `dspi_link_counters()`.

Tests: `test/host/test_dspi_link.c` drives the link engine against
`test/host/fake_dspi.c`, a simulated DSPi on a virtual clock that can stay
silent, answer late, corrupt its CRC, return any status, put noise on the
line or fail a restart.
