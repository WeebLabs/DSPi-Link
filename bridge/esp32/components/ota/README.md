# ota

The bridge's own firmware updates and rollback (bridge spec 14.2).

| File | What it does | Portable |
|------|--------------|----------|
| `include/ota.h`, `ota.c` | The ota task: writes an image into the inactive app partition chunk by chunk, hashes it as it goes, checks the SHA-256, the chip and that it is bridge firmware, and makes it the image that starts next. Also confirms a new image after a healthy first minute. | no |

The hub's side of an update, `hub.update` and its FWDATA chunks, is in
`hub/hub_update.c`; the hub task connects the two.

Public API: `ota_start()`, `ota_capacity()`, `ota_begin()`, `ota_write()`,
`ota_abort()`, `ota_confirm_after_healthy_start()`.

Tests: the hub's half has host tests in `test/host/test_hub.c`. This half
needs flash, so it is checked on a board: an update, a confirmed image
surviving a restart, an image that crashes rolling back, and wrong files
being refused.
