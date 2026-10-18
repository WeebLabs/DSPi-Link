# DSPi Link ESP32 bridge

Firmware that turns a common ESP32 board into a DSPi Link hub: wired to a
DSPi's UART and joined to Wi-Fi, it lets DSPi Console and the browser control
that DSPi over the network. What it does and why is in
[`spec/esp32_bridge_spec.md`](../../spec/esp32_bridge_spec.md); the build
order is in [`PLAN.md`](PLAN.md).

**Status:** phase 3 of 8. The bridge is a working Link hub: it finds the
wired DSPi, joins Wi-Fi, advertises itself by DNS-SD, and serves DSPi
Console and the web client over the Link WebSocket, with the `auth: none`
default. Until phase 4 adds the setup page, it joins the network named in a
local file (see below).

Supported chips: ESP32, ESP32-S3, ESP32-C3 and ESP32-C6, with at least 4 MB
of flash.

## Build

The firmware embeds the web pages, so build them first, once and after any
change under `js/`:

```bash
cd ../../js && npm ci && npm run build
```

Until phase 4, the bridge joins the Wi-Fi network named in
`components/net/local_wifi.h`. Copy the template and fill it in; the copy is
git-ignored:

```bash
cp components/net/local_wifi.example.h components/net/local_wifi.h
```

Builds run in Espressif's official Docker image of the ESP-IDF version in
[`idf_version.txt`](idf_version.txt), so only Docker and Node are needed:

```bash
tools/build.sh
```

That builds all four chips into `build/<chip>/`. To build one:

```bash
tools/build.sh esp32s3
```

## Flash

Flashing runs on your computer, because a container cannot reach the USB
port on macOS or Windows. Install esptool once:

```bash
python3 -m pip install esptool
```

Then, from the chip's build folder, with your board's serial port:

```bash
cd build/esp32s3 && python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 write-flash @flash_args
```

To start again from factory state, erase the flash first:

```bash
python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 erase-flash
```

## Test

The portable components have host tests that run without ESP-IDF. The first
configure downloads cJSON, the same release the firmware uses:

```bash
cmake -S test/host -B build-host && cmake --build build-host && ctest --test-dir build-host --output-on-failure
```

A built image can also be booted twice in Espressif's QEMU, which checks the
boot sequence and flash storage without a board. Each boot stops before
Wi-Fi, since QEMU has no radio. It runs for the ESP32, ESP32-S3 and ESP32-C3;
QEMU has no ESP32-C6:

```bash
tools/qemu_smoke.sh esp32s3
```

On a board without PSRAM, or in QEMU, ESP-IDF logs two `esp_psram` errors at
boot. They are expected: the image enables PSRAM where a board has it and
carries on without it.

With a bridge on the network, `tools/link_smoke.mjs` drives it end to end
through the repository's own client library. It nudges the master volume by
0.5 dB and puts it back:

```bash
node tools/link_smoke.mjs dspi-bridge-1b34.local
```

C sources are formatted with the committed `.clang-format`, checked in CI
with clang-format 23.1.3. The generated policy table is left alone:

```bash
clang-format --dry-run --Werror $(find components main test -name '*.[ch]' ! -name link_policy_table.c)
```

`components/link_policy/link_policy_table.c` is generated from
`policy/commands.json`. After changing the JSON, regenerate it; CI checks it
is current:

```bash
python3 tools/gen_policy.py
```

## Layout

| Path | What it is |
|------|------------|
| `main/app_main.c` | The boot sequence, and nothing else. |
| `components/bridge_config/` | Every tunable number, with its unit and reason. |
| `components/board/` | The pins each chip may use, and PSRAM detection. |
| `components/settings/` | The saved settings: their rules and their storage in flash. |
| `components/dspi_frame/` | The DSPi's UART frames and their CRC. |
| `components/dspi_uart/` | The link engine and the uart task that owns the UART. |
| `components/link_policy/` | Which role may send which command, generated from `policy/commands.json`. |
| `components/link_codec/` | Link binary frames and base64. |
| `components/hub/` | The Link hub state machine, and the hub task that runs it. |
| `components/http_server/` | The Link WebSocket, the info endpoint, the web pages and the port 80 redirect. |
| `components/net/` | Wi-Fi and the DNS-SD advertisement. |
| `components/web_assets/` | The built web pages, gzipped into the firmware at build time. |
| `test/host/` | Host tests for the portable components. |
| `tools/build.sh` | The Docker build used locally and in CI. |
| `tools/qemu_smoke.sh` | Boots a built image twice in QEMU, used locally and in CI. |
| `tools/gen_policy.py` | Generates the policy table from `policy/commands.json`. |
| `tools/embed_web.py` | Turns `js/apps/web/dist` into C at build time. |
| `tools/link_smoke.mjs` | Drives a live bridge end to end with `dspi-link-js`. |

Section 6.3 of the bridge spec lists the components still to come.
