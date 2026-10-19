# settings

The bridge's saved state (bridge spec 11). This is the only component that
touches flash storage (NVS).

| File | What it does | Portable |
|------|--------------|----------|
| `include/settings_model.h`, `settings_model.c` | The settings struct, factory defaults, and the validity rules of bridge spec 8.11, with repair of invalid values. | yes |
| `include/settings.h`, `settings.c` | Initialises NVS and stores the four namespaces: `bridge` (one key per setting, repaired and re-saved on load), `devices` (names by serial), `wifi` (the home network, its name and password as one blob so they are always written together) and `auth` (the PIN and paired clients as one blob). Also the factory reset. | no |

Public API:

- Model: `settings_model_defaults()`, `settings_model_find_problem()`,
  `settings_model_repair()`, and the checks `settings_name_is_valid()`,
  `settings_hostname_is_valid()`, `settings_hub_id_is_valid()` and
  `settings_baud_is_supported()`.
- Storage: `settings_init()`, `settings_load()`, `settings_save()`,
  `settings_load_device_name()`, `settings_save_device_name()`,
  `settings_load_wifi()`, `settings_save_wifi()`, `settings_forget_wifi()`,
  `settings_load_auth()`, `settings_save_auth()` and `settings_erase_all()`.

Tests: `test/host/test_settings_model.c` covers defaults, every validation
rule, repair, and repair refusing defaults that are themselves invalid. The
NVS side runs only on a device; `tools/qemu_smoke.sh` checks that settings
survive a reboot.
