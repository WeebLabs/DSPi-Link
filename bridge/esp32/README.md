# DSPi Link ESP32 bridge

Firmware that turns a common ESP32 board into a DSPi Link hub. Wired to a
DSPi's UART and joined to your Wi-Fi, it lets DSPi Console and a browser
control that DSPi over the network. What it does and why is in
[`spec/esp32_bridge_spec.md`](../../spec/esp32_bridge_spec.md); the build
order and the state of each part are in [`PLAN.md`](PLAN.md).

**Status:** phase 7 of 8, ready for release testing. The bridge sets itself
up from a phone, serves DSPi Console and the browser, has an optional PIN,
a settings page, firmware updates with automatic rollback, and BOOT button
resets.

Supported chips: ESP32, ESP32-S3, ESP32-C3 and ESP32-C6, with at least 4 MB
of flash.

## Install

The easiest way is the installer page,
[weeblabs.github.io/DSPi-Link/installer](https://weeblabs.github.io/DSPi-Link/installer/):
open it in Chrome or Edge on a computer, plug in the board and press
Install. The page also shows the wiring for your chip. The rest of this
section says the same in more detail.

### 1. Wire the board to the DSPi

| ESP32 board | DSPi (DSPi Console's defaults) |
|-------------|--------------------------------|
| UART TX pin, from the table below | GPIO 17 (UART RX) |
| UART RX pin, from the table below | GPIO 16 (UART TX) |
| GND | GND |

| Chip | UART TX | UART RX | BOOT button |
|------|---------|---------|-------------|
| ESP32 | GPIO 19 | GPIO 18 | GPIO 0 |
| ESP32-S3 | GPIO 17 | GPIO 18 | GPIO 0 |
| ESP32-C3 | GPIO 5 | GPIO 4 | GPIO 9 |
| ESP32-C6 | GPIO 22 | GPIO 23 | GPIO 9 |

TX always goes to RX. Keep the wires short, under 30 cm. Both boards use
3.3 V logic, so no level shifter is needed. Other pins can be chosen later
on the settings page; bridge spec section 4.4 lists which pins each chip
allows and why the rest are left out.

Power the ESP32 board from its own USB socket or a 5 V supply, never from
the DSPi's 3.3 V pin: Wi-Fi draws current peaks the DSPi's regulator cannot
supply, and the brownouts look like random crashes on both boards. The
ground wire is still needed.

### 2. Turn on the DSPi's UART

The DSPi accepts this setting only over USB, so set it once in DSPi Console
with the DSPi plugged in. The DSPi keeps it, even through its own factory
reset.

| Setting | Value |
|---------|-------|
| UART control interface | On |
| TX pin / RX pin | GPIO 16 / GPIO 17 |
| Baud rate | 1,000,000 |
| Notifications | On |

### 3. Install the firmware

Use the installer page, or flash a release by hand with esptool (below).
The installer erases the board first.

### 4. Join it to your Wi-Fi

On first power-up the bridge shows an open Wi-Fi network named
`DSPi Bridge XXXX`. Join it from a phone and the setup page opens by itself;
if it does not, browse to `http://192.168.4.1/`. Choose your 2.4 GHz home
network, enter its password and a name, and choose whether to protect the
bridge with a PIN. The page shows the bridge's address once it has joined.

### 5. Use it

DSPi Console finds the bridge by itself. In a browser, open the address the
setup page showed, such as `http://dspi-bridge-1b34.local:11915`. Choose
**Bridge settings** there for the DSPi connection, Wi-Fi, the PIN, the name
and hostname, the status LED, firmware updates, restart and resets.

If the home network disappears for 3 minutes, the setup network comes back
by itself, so the bridge can be moved to another network from a phone.

## BOOT button and LED

| Hold BOOT, then let go | What happens |
|------------------------|--------------|
| under 5 s | Nothing. |
| 5 to 15 s | Forget Wi-Fi: the bridge restarts into its setup network. Everything else is kept. |
| 15 to 30 s | Factory reset: everything is erased, including the PIN. |
| over 30 s | Nothing: the hold is cancelled. |

A factory reset is the only way out of a forgotten PIN, deliberately.
Holding BOOT while plugging the board in starts the chip's download mode
instead, which is how firmware is installed.

An optional status LED shows what the bridge is doing (bridge spec 13):
a slow blue pulse while its setup network is up, a fast blue blink while
joining Wi-Fi, green for 5 s once it is online with the DSPi, a slow amber
blink if the DSPi is not answering, and the BOOT stages above in amber and
red. Choose the LED's type and pin under **Bridge settings**. Boards without
one can follow the same stages in the serial log.

## Update

Download `dspi-bridge-<version>-<chip>-update.bin` from the
[releases](https://github.com/WeebLabs/DSPi-Link/releases), then in **Bridge
settings**, under Maintenance, choose **Update firmware** and pick it. The
page refuses a file for another chip before sending it. The bridge checks
the file again, restarts into it, and keeps it once it has run properly for
a minute; if it does not, the bridge goes back to the firmware it had.
Settings, Wi-Fi, the PIN and paired apps are kept.

## Troubleshooting

**The setup network does not appear.** It appears only while no home
network is saved, or after the saved one has been missing for 3 minutes.
To forget a saved network, hold BOOT for 5 to 15 s and let go.

**The phone joined the setup network but no page opened.** Browse to
`http://192.168.4.1/`. Some phones offer to "stay connected" to a network
without internet; say yes.

**My network is not in the list.** ESP32 boards use 2.4 GHz only. A router
that offers both bands under one name works; a 5 GHz-only network does not.
For a hidden network, choose "Enter a network name".

**The setup page says the password did not work.** Check it and try again;
the bridge returns to the setup network after a refusal.

**"DSPi not found" or "DSPi not answering".** In order of likelihood: the
DSPi's UART control interface is off (step 2), TX and RX are swapped, the
ground wire is missing, or the DSPi's UART was moved to other pins in
Console. A wrong baud rate fixes itself: the bridge searches the usual rates
when the DSPi does not answer.

**"Notifications are off".** Turn on Notifications in DSPi Console's UART
settings. The bridge works without them, but changes made elsewhere show up
more slowly.

**DSPi Console does not find the bridge.** Console finds it by DNS-SD,
which does not cross between networks: a guest Wi-Fi, a separate VLAN or
"client isolation" on the access point all hide it. Put the computer and
the bridge on the same network, or connect to the bridge's IP address.

**The `.local` address does not open.** Some systems, older Windows and
Android among them, do not resolve `.local` names. Use the IP address the
setup page showed, or look the bridge up in your router.

**I forgot the PIN.** Hold BOOT for 15 to 30 s and let go, then set the
bridge up again. There is deliberately no other way.

**The bridge restarts at random.** It is almost always power: run the board
from its own USB supply, not from the DSPi.

**The installer finds no port.** Try another USB cable, since some carry
power only. Older boards with a CP210x or CH340 USB chip may need its
driver. If the port appears but installing fails, hold BOOT while plugging
the board in.

**An update was refused.** The page and the bridge say why in a sentence.
"The bridge has only just been updated" means the last update is still in
its first minute; try again shortly.

**Reading the serial log.** Connect the board by USB and open its serial
port at 115200 baud, for example with `python3 -m serial.tools.miniterm`.
The first lines after a restart say the firmware version, chip, hub name,
pins and baud rate, which answers most questions.

## Build

The firmware embeds the web pages, so build them first, once and after any
change under `js/`:

```bash
cd ../../js && npm ci && npm run build
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

## Flash by hand

Flashing runs on your computer, because a container cannot reach the USB
port on macOS or Windows. Install esptool once:

```bash
python3 -m pip install esptool
```

To flash your own build, from the chip's build folder, with your board's
serial port:

```bash
cd build/esp32s3 && python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 write-flash @flash_args
```

To flash a release's full image instead, at address 0:

```bash
python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 write-flash 0x0 dspi-bridge-1.0.0-esp32s3.bin
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
QEMU has no ESP32-C6. A second argument boots a release's full image:

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

## Release

The version is `PROJECT_VER` in [`CMakeLists.txt`](CMakeLists.txt). To
release, set it, commit, and push a tag named after it:

```bash
git tag bridge-v1.0.0 && git push origin bridge-v1.0.0
```

The release workflow (`.github/workflows/esp32-bridge-release.yml`) builds
every chip, packages each one as a full image for first installation and an
update image, boots the full images in QEMU, attaches them to a GitHub
release with their SHA-256 sums, and publishes the installer page to GitHub
Pages. GitHub Pages must be set once to deploy from GitHub Actions
(repository Settings, Pages, Source).

To package locally after `tools/build.sh`, which writes `build/release/` and
the installer site in `build/site/`:

```bash
tools/package_release.sh
```

## Layout

| Path | What it is |
|------|------------|
| `main/app_main.c` | The boot sequence, and the few lines that connect components. |
| `components/bridge_config/` | Every tunable number, with its unit and reason. |
| `components/board/` | The pins each chip may use, and PSRAM detection. |
| `components/settings/` | The saved settings: their rules and their storage in flash. |
| `components/dspi_frame/` | The DSPi's UART frames and their CRC. |
| `components/dspi_uart/` | The link engine and the uart task that owns the UART. |
| `components/link_policy/` | Which role may send which command, generated from `policy/commands.json`. |
| `components/link_codec/` | Link binary frames and base64. |
| `components/hub/` | The Link hub state machine, its administration and update messages, and the hub task that runs it. |
| `components/hub_auth/` | The PIN, paired clients' tokens and the pairing rate limits. |
| `components/http_server/` | The Link WebSocket, the info endpoint, the web pages and the port 80 redirect. |
| `components/net/` | The Wi-Fi state machine, the setup network and its DNS answers, and the DNS-SD advertisement. |
| `components/web_assets/` | The built web pages, gzipped into the firmware at build time. |
| `components/setup_api/` | The setup network's page and API. |
| `components/ota/` | Firmware updates and rollback. |
| `components/button/` | The BOOT button's long presses. |
| `components/status_led/` | The optional status LED. |
| `installer/` | The installer page, published to GitHub Pages with each release. |
| `test/host/` | Host tests for the portable components. |
| `tools/build.sh` | The Docker build used locally and in CI. |
| `tools/qemu_smoke.sh` | Boots an image twice in QEMU, used locally and in CI. |
| `tools/package_release.sh` | Makes the release files and the installer site. |
| `tools/gen_policy.py` | Generates the policy table from `policy/commands.json`. |
| `tools/embed_web.py` | Turns `js/apps/web/dist` into C at build time. |
| `tools/link_smoke.mjs` | Drives a live bridge end to end with `dspi-link-js`. |

Each component's own `README.md` lists its files and public functions.
