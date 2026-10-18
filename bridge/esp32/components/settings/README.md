# settings

The bridge's saved settings (bridge spec 11). This is the only component that
touches flash storage (NVS).

| File | What it does | Portable |
|------|--------------|----------|
| `include/settings_model.h`, `settings_model.c` | The settings struct, factory defaults, and the validity rules of bridge spec 8.11. | yes |
| `include/settings.h`, `settings.c` | Initialises NVS, and loads and saves the `bridge` namespace one key per field, repairing invalid values on load. | no |

Public API: `settings_init()`, `settings_load()`, `settings_save()`, plus the
model functions `settings_model_defaults()`, `settings_model_find_problem()`
and `settings_model_repair()`.

The `wifi`, `auth` and `devices` namespaces join in later phases.

Tests: `test/host/test_settings_model.c` covers defaults, every validation
rule and repair.
