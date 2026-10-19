# dspi_frame

The DSPi's UART frames (bridge spec 7.1, firmware
`control_interfaces_spec.md` section 3): building requests, parsing responses
and notifications byte by byte, and the CRC-16/CCITT-FALSE that guards them.

| File | What it does | Portable |
|------|--------------|----------|
| `include/dspi_frame.h`, `dspi_frame.c` | `dspi_frame_encode_request()` builds a request's header and CRC without copying its payload; `dspi_parser_feed()` takes one received byte at a time and reports a finished frame, a bad CRC, or a frame too long for the buffer; `dspi_parser_rescan()` searches a discarded or abandoned (`dspi_parser_abandon()`) frame's bytes again, so a false start cannot swallow a real frame; `dspi_crc16_update()` computes the CRC in pieces. | yes |

The parser never allocates: the caller gives it the buffer, which holds a
whole frame after its sync byte (`DSPI_PARSER_BUFFER_BYTES()`).

Tests: `test/host/test_dspi_frame.c`, including the firmware spec's CRC check
value and its master-volume example frame. `test/host/test_frames.c` builds
device frames with a second, independent CRC so the two check each other.
