# DSPi Link ESP32 bridge

Firmware that turns a common ESP32 board into a DSPi Link hub: wired to a
DSPi's UART and joined to Wi-Fi, it lets DSPi Console and the browser control
that DSPi over the network. What it does and why is in
[`spec/esp32_bridge_spec.md`](../../spec/esp32_bridge_spec.md); the build
order is in [`PLAN.md`](PLAN.md).

**Status:** phase 2 of 8. The firmware finds a wired DSPi over UART,
detecting its baud rate, reads its identity and settings blob, and logs
every notification it sends. It has no network side yet.

Supported chips: ESP32, ESP32-S3, ESP32-C3 and ESP32-C6, with at least 4 MB
of flash.

## Build

Builds run in Espressif's official Docker image of the ESP-IDF version in
[`idf_version.txt`](idf_version.txt), so only Docker is needed:

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

The portable components have host tests that run without ESP-IDF:

```bash
cmake -S test/host -B build-host && cmake --build build-host && ctest --test-dir build-host --output-on-failure
```

A built image can also be booted twice in Espressif's QEMU, which checks the
boot sequence and flash storage without a board (ESP32, ESP32-S3 and
ESP32-C3 only):

```bash
tools/qemu_smoke.sh esp32s3
```

On a board without PSRAM, or in QEMU, ESP-IDF logs two `esp_psram` errors at
boot. They are expected: the image enables PSRAM where a board has it and
carries on without it.

C sources are formatted with the committed `.clang-format`, checked in CI
with clang-format 23.1.3:

```bash
clang-format --dry-run --Werror $(find components main test -name '*.[ch]')
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
| `main/phase2_bench.c` | Temporary: logs what the uart task finds, until the hub replaces it in phase 3. |
| `test/host/` | Host tests for the portable components. |
| `tools/build.sh` | The Docker build used locally and in CI. |
| `tools/qemu_smoke.sh` | Boots a built image twice in QEMU, used locally and in CI. |

Section 6.3 of the bridge spec lists the components still to come.
