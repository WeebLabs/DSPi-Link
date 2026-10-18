# board

Which pins the bridge may use on each chip (bridge spec 4.4), and whether
the board has PSRAM (bridge spec 4.1).

| File | What it does | Portable |
|------|--------------|----------|
| `include/board_table.h`, `board_tables.c` | One table per chip: default UART pins, the BOOT button, and the pins offered for UART TX, UART RX and the LED, with the reason each other pin is left out. | yes |
| `include/board.h`, `board.c` | Picks the table for the chip the image was built for, reports PSRAM and the session limit. | no |

Public API: `board_table()`, `board_has_psram()`, `board_max_sessions()`,
`board_pin_list_contains()`.

Tests: `test/host/test_board_tables.c` checks every table against the
datasheet exclusions, restated independently in the test.
