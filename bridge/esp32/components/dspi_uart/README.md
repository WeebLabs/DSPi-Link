# dspi_uart

The link to the DSPi over UART (bridge spec 7) and the uart task that owns it
(bridge spec 6.2).

| File | What it does | Portable |
|------|--------------|----------|
| `include/dspi_link.h`, `dspi_link.c` | The link engine: one request at a time with the idle gap, timeouts sized to the baud rate, retries, quarantine after a timeout, baud detection and reading the device's identity. Reaches the UART and clock only through `dspi_port_t`. | yes |
| `include/dspi_uart.h`, `dspi_uart.c` | The uart task: supplies the real ESP32 UART as a `dspi_port_t`, finds the DSPi, runs queued requests, relays notifications, sends heartbeats, and reports the device found or lost. | no |

Public API: `dspi_uart_start()` and `dspi_uart_submit()`, with results
delivered to the handlers in `dspi_uart_config_t`. Handlers run on the uart
task and must copy what they keep.

Tests: `test/host/test_dspi_link.c` drives the link engine against
`test/host/fake_dspi.c`, a simulated DSPi on a virtual clock that can stay
silent, answer late, corrupt its CRC or return any status.
