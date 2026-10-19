# DSPi Link ESP32 Bridge Specification

*Version: 1.0 (draft)*
*Implements: DSPi Link protocol 1.2 (`spec/dspi_link_protocol_spec.md`)*
*Status: draft; no implementation yet. `bridge/esp32/PLAN.md` gives the build order.*
*Last updated: 2026-10-18*

The ESP32 bridge is the reference implementation of a DSPi Link bridge hub.
It is a firmware image for common ESP32 boards. Wired to a DSPi's UART
control interface and joined to a home Wi-Fi network, it makes that DSPi
available to DSPi Console, the browser and any other Link client on the
network, with no computer left running.

This document says what the bridge does and how it is built. The protocol
spec says what goes over the network; where the two overlap, the protocol
spec decides and this document only records the bridge's choices. "Link
spec 7.10" below means section 7.10 of the protocol spec. "Firmware spec"
means `Documentation/Features/control_interfaces_spec.md` in the DSPi
firmware repository.

Because this is a reference implementation, clarity is a requirement on the
same level as correctness. Section 16 sets the code standards.

---

## 1. Scope

The bridge:

- serves one DSPi, connected over UART;
- implements a Link 1.2 hub of kind `bridge` with the capabilities in
  section 8.1;
- joins a 2.4 GHz Wi-Fi network, set up from a phone or computer through a
  setup network and captive portal;
- serves the DSPi control web page and its own settings page;
- can be protected with an optional PIN, off by default;
- updates its own firmware from the settings page, with automatic rollback;
- resets to its network setup or to factory state from the BOOT button or
  the settings page.

Out of scope for version 1.0:

- I2C. The DSPi's I2C interface cannot push notifications and has no
  checksum, so UART is the only supported link.
- Wi-Fi setup over Bluetooth or Improv.
- TLS, static IP addresses and 5 GHz Wi-Fi.
- More than one DSPi per bridge.
- Updating the DSPi's own firmware. The DSPi's bootloader is reachable only
  over USB, so the bridge omits `fw_install`.
- Enabling the DSPi's UART interface. The firmware accepts that command only
  over USB, so the user does it once in DSPi Console (section 5).

---

## 2. Terms

| Term | Meaning |
|------|---------|
| Bridge | The ESP32 running this firmware. |
| Home network | The user's Wi-Fi network, which the bridge joins as a station. |
| Setup network | The open Wi-Fi network the bridge creates when it has no home network to join, named `DSPi Bridge XXXX`. |
| Setup page | The page a phone or computer sees on the setup network. It joins the bridge to a home network. |
| Control page | The DSPi web client from `js/apps/web`, served at `/`. It controls the DSPi. |
| Settings page | The bridge's settings screen inside the control page, shown when the hub has the `hub_config` capability. |
| PIN | The optional 6-digit fixed PIN (Link spec 6.2). When it is on, every browser and app enters it once. |
| `XXXX` | The last two bytes of the ESP32's Wi-Fi MAC address in upper-case hex, which tell several bridges apart. |

---

## 3. The user's journey

1. **Prepare the DSPi.** With the DSPi plugged into a computer, the user
   opens DSPi Console and turns on the UART control interface at 1,000,000
   baud with notifications on (section 5). This is a one-time step.
2. **Install the firmware.** The user opens the bridge's installer web page
   in Chrome or Edge, plugs the ESP32 in over USB and clicks Install. The
   page detects the chip and writes the matching image (section 15).
3. **Wire it up.** Three wires: ESP32 TX to DSPi RX, ESP32 RX to DSPi TX, and
   ground to ground (section 4.2). The ESP32 gets its own 5 V supply.
4. **Join the setup network.** On first power-up the bridge creates the
   network `DSPi Bridge XXXX`. When a phone joins it, the setup page opens by
   itself.
5. **Set up.** The setup page shows whether the DSPi was found, lists nearby
   networks, takes the Wi-Fi password, a name for the bridge and the optional
   PIN, and joins the home network. It then shows the bridge's new address.
6. **Use it.** DSPi Console finds the bridge on the home network by itself.
   The control page and settings page are at
   `http://dspi-bridge-xxxx.local:11915/`, and plain `http://dspi-bridge-xxxx.local/`
   redirects there.

If the home network later disappears, for example after a router change, the
bridge brings the setup network back by itself after 3 minutes (section 9.1).
Moving to a new network therefore needs no reset.

---

## 4. Hardware

### 4.1 Supported chips

| Chip | Typical boards | Notes |
|------|----------------|-------|
| ESP32 | ESP32-DevKitC (WROOM and WROVER), most "ESP32 dev board" clones | The original. 520 KB RAM, some boards add PSRAM. |
| ESP32-S3 | ESP32-S3-DevKitC-1, Seeed XIAO ESP32S3, M5Stack AtomS3 | The recommended chip. Most boards have PSRAM. |
| ESP32-C3 | ESP32-C3-DevKitM-1, "C3 SuperMini", Seeed XIAO ESP32C3 | Single core, 400 KB RAM. |
| ESP32-C6 | ESP32-C6-DevKitC-1, Seeed XIAO ESP32C6 | Single core, Wi-Fi 6 on 2.4 GHz. |

The bridge needs at least 4 MB of flash, which every board above has.
Without PSRAM the bridge allows 4 simultaneous Link sessions; with PSRAM it
allows 8 (section 8.2).

### 4.2 Wiring

| ESP32 | DSPi (Console defaults) | Note |
|-------|-------------------------|------|
| UART TX pin (section 4.4) | GPIO 17 (UART RX) | TX always goes to RX. |
| UART RX pin (section 4.4) | GPIO 16 (UART TX) | RX always goes to TX. |
| GND | GND | Required. Without a shared ground the link does not work. |

Both boards use 3.3 V logic, so no level shifter is needed. At 1 Mbaud the
wires should be short, ideally under 30 cm. If the user moved the DSPi's
UART to other pins in Console, the ESP32 side is unchanged and only the DSPi
end of the wires moves.

### 4.3 Power

The ESP32 must not be powered from the DSPi's 3.3 V pin. Wi-Fi transmission
draws current peaks of around 500 mA, beyond what the regulator on a Pico
board supplies, and the resulting brownouts look like random crashes on
both boards. The ESP32 is powered from its own USB socket or from a 5 V
supply to its 5 V pin. The ground wire in section 4.2 is still required.

### 4.4 Pins on each chip

The ESP32 can route its UART to almost any pin, so the user chooses the
pins in the settings page. The bridge offers only pins that are safe on the
chip it is running on. It never offers flash and PSRAM pins, USB pins, the
console UART, or strapping pins (pins whose level at power-up chooses the
boot mode, which a DSPi driving them could upset).

| Chip | Default TX | Default RX | BOOT button | Offered for the UART |
|------|-----------|-----------|-------------|----------------------|
| ESP32 | GPIO 19 | GPIO 18 | GPIO 0 | 4, 13, 14, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33 |
| ESP32-S3 | GPIO 17 | GPIO 18 | GPIO 0 | 1, 2, 4 to 18, 21, 38 to 42 |
| ESP32-C3 | GPIO 5 | GPIO 4 | GPIO 9 | 0, 1, 3, 4, 5, 6, 7, 10 |
| ESP32-C6 | GPIO 22 | GPIO 23 | GPIO 9 | 0, 1, 2, 3, 6, 7, 18 to 23 |

Pins left out, and why:

- **ESP32:** 0, 2, 5, 12 and 15 are strapping pins; 1 and 3 are the boot
  console; 6 to 11 connect to flash; 16 and 17 connect to PSRAM on WROVER
  modules and to flash on PICO-D4 modules; 34 to 39 are input only and
  have no pull-up, so an RX line on one would float and read noise whenever
  the DSPi is unplugged or off; 37 and 38 are also not broken out on WROOM
  or WROVER modules. A saved pin the chip no longer offers fails validation,
  and both UART pins return to the defaults (section 11).
- **ESP32-S3:** 0, 3, 45 and 46 are strapping pins; 19 and 20 are USB; 26 to
  32 connect to flash and PSRAM; 33 to 37 connect to octal flash or PSRAM on
  many modules; 43 and 44 are the boot console; 47 and 48 run at 1.8 V on
  modules with 1.8 V octal PSRAM, too low for the DSPi to read.
- **ESP32-C3:** 2, 8 and 9 are strapping pins; 11 supplies the flash on most
  modules; 12 to 17 connect to flash; 18 and 19 are USB; 20 and 21 are the
  boot console.
- **ESP32-C6:** 4, 5, 8, 9 and 15 are strapping pins; 12 and 13 are USB; 16
  and 17 are the boot console; 24 to 30 connect to flash; 10, 11 and 14 are
  not broken out on the ESP32-C6-WROOM-1.

The LED may also use a few pins left out above, where common boards fit
their LED: 2 on the ESP32, 48 on the ESP32-S3 (DevKitC-1 v1.0), 8 on the
ESP32-C3 and ESP32-C6, and 15 on the ESP32-C6 (XIAO). Driving an LED after
boot cannot upset a strapping pin.

The defaults are broken out on the development boards in section 4.1
(DevKitC and DevKitM). Smaller boards such as the Seeed XIAO and M5Stack
AtomS3 break out fewer pins: wire the DSPi to two of the pins in the lists
above that the board does break out, and choose them on the settings page,
which offers exactly these lists. The `board` component (section 6.4) holds these lists as the single
source of truth, and the host tests check them against the exclusions
above. The bridge always uses UART1, which every
supported chip has, and leaves UART0 to the boot console.

A status LED is optional and off by default (section 13). When the user
enables one, its pin is removed from the UART choices, and the UART pins are
removed from the LED choices.

---

## 5. Preparing the DSPi

The DSPi ships with its UART control interface off, and the firmware
accepts the command that turns it on (`REQ_SET_UART_CONFIG`, 0xF5) only over
USB. The user therefore sets it once in DSPi Console with the DSPi plugged
in:

| Setting | Value |
|---------|-------|
| UART control interface | On |
| TX pin / RX pin | GPIO 16 / GPIO 17 (the defaults), or any valid pair |
| Baud | 1,000,000 |
| Notifications | On |

The DSPi saves this setting, and it survives reboots and DSPi factory
resets.

The bridge checks the result rather than trusting it:

- **No answer at any baud rate.** The UART is off, the wires are swapped or
  missing, or ground is not connected. The setup and settings pages say
  exactly this and link to the wiring diagram.
- **Notifications off.** The bridge reads `REQ_GET_UART_CONFIG` (0xF6) and
  sees `notify_enable = 0`. It works, but omits the `notify` capability, so
  clients fall back to polling and react more slowly. The pages show a
  warning explaining how to turn notifications on in Console.
- **Slow baud rate.** Below 921600 baud the bridge works, but bulk reads take
  longer (530 ms at 115200) and the poll budget shrinks. The pages recommend
  1,000,000.

---

## 6. Firmware architecture

### 6.1 Framework

The firmware is built with ESP-IDF, Espressif's own framework, version 6.1.
The version is pinned in one place (`bridge/esp32/idf_version.txt`) and used
by the build script and CI, which both run Espressif's official Docker image
of that version. Components from the ESP-IDF component registry are pinned
to exact versions in the `idf_component.yml` of the component that uses
them, and the host tests fetch the same cJSON release.

ESP-IDF components used:

| Component | Used for |
|-----------|----------|
| `esp_wifi`, `esp_netif`, `esp_event` | Station and setup-network Wi-Fi. |
| `esp_http_server` | HTTP, the WebSocket and the setup API on ports 11915 and 80. |
| `espressif/mdns` (component registry) | DNS-SD advertisement and the `.local` hostname. |
| `nvs_flash` | Saved settings. |
| `esp_driver_uart` | The DSPi link. |
| `espressif/cjson` (component registry) | The JSON control plane. ESP-IDF 6 moved cJSON out of the framework. |
| `mbedtls` | SHA-256 and PBKDF2 for tokens and the PIN. |
| `app_update`, `bootloader_support` | Firmware updates with rollback. |
| `esp_sntp` | Wall-clock time for the paired-client list. |
| `led_strip` (managed component) | Addressable status LEDs. |

### 6.2 Tasks and ownership

The firmware runs three tasks of its own, and each piece of state has
exactly one owner. Only three small things are shared between tasks, each
behind a lock held for a few instructions: the HTTP server's table of
session slots, the cached `/dspi/v1/info` document, and the Wi-Fi status.

```
                +-----------------------------+
 HTTP task      |  esp_http_server            |  WebSocket frames, HTTP requests
 (ESP-IDF)      |  parses, then posts events  |-----+
                +-----------------------------+     |
                                                    v
                +-----------------------------+   hub event queue
 hub task       |  hub: sessions, auth,       |<----+
                |  routing, polls, locks,     |<----+
                |  notify relay, config       |     |
                +-----------------------------+     |
                        | uart request queue        |
                        v                           |
                +-----------------------------+     |
 uart task      |  dspi_uart: framing, timing,|-----+  responses, notifications,
                |  retries, presence, baud    |        device found / lost
                +-----------------------------+
```

- **The hub task owns all Link state.** Every change to sessions,
  subscriptions, locks, the device record and the configuration happens on
  this task, in response to one event at a time. This makes the hub a plain
  state machine that can be read top to bottom and tested on a computer.
- **The uart task owns the UART.** It takes one request at a time from its
  queue, runs it to completion and posts the result back. It also reads
  notifications between requests and probes for the device. The UART's
  interrupt runs from internal RAM (`CONFIG_UART_ISR_IN_IRAM`), so it keeps
  receiving while flash is being written, during an update or a settings
  save.
- **The HTTP task belongs to ESP-IDF.** Handlers on it only copy input and
  post events to the hub task; they never touch hub state. Outbound frames
  wait in each session's queue, and one job on the HTTP task sends them in
  batches without blocking (section 8.2). Closing a socket also goes
  through that task; a request that finds ESP-IDF's small work queue full
  is kept and tried again on the next hub tick, so no close is ever lost.
- **A session's slot is reused only once the hub has seen it close.** The
  HTTP server marks a slot closing when its socket closes, and frees it
  when the hub task has handled the close. A new connection can therefore
  never receive frames the hub meant for the old one.
- **Wi-Fi, the button and the LED** run from ESP-IDF event callbacks and a
  timer. They post events to the hub task or call the `net` module's
  functions, which are safe to call from any task. The button never waits
  for the hub task: if the hub task cannot take a reset at once, the button
  erases and restarts by itself, without telling the sessions.
- **Events carry no heap memory.** Answers and notifications reach the hub
  task in buffers from a fixed pool. A notification that finds the pool
  empty is dropped (and the sessions get RESYNC); an answer that finds it
  empty still reaches its client, as a timeout. Part of the hub task's
  queue is kept for the events that must never be lost: sessions opening
  and closing, the DSPi appearing and going, update progress and restart.
- **The uart task never waits on the hub.** It posts answers and
  notifications without waiting; a dropped answer is finished as a timeout
  by the hub. A DSPi appearing or going waits a few milliseconds at most,
  and otherwise the hub picks the latest change up on its next tick. Drops
  are counted and logged together every few seconds, never one line each.
- **No session can crowd out the others.** Each session may have a few
  messages (8, and 16 KB) waiting for the hub at once; a session over its
  share is held back for up to 2 s, then closed with code 1013.
- **No answer holds the queue.** If the uart task's answer to the oldest
  request has not arrived well after the uart task's own timeouts, the hub
  finishes that request as a timeout and ignores a late answer.

### 6.3 Repository layout

The bridge lives in this repository, next to the spec, policy table and
fixtures it is tested against.

```
bridge/esp32/
  README.md                 Build, flash and test instructions
  PLAN.md                   Implementation order
  idf_version.txt           The pinned ESP-IDF version
  CMakeLists.txt            ESP-IDF project file
  partitions.csv            Section 14.1
  sdkconfig.defaults        Settings shared by every chip
  sdkconfig.defaults.esp32  One file per chip, only what differs
  sdkconfig.defaults.esp32s3
  sdkconfig.defaults.esp32c3
  sdkconfig.defaults.esp32c6
  main/
    app_main.c              Boot order only (section 6.5); no logic
  components/
    bridge_config/          Every tunable constant, with units, in one header
    link_codec/             Link binary frames and base64             [portable]
    link_policy/            Authorization table generated from policy/commands.json [portable]
    dspi_frame/             DSPi UART frame encoder, parser and CRC   [portable]
    dspi_uart/              The link engine [portable] and the uart task around it
    hub/                    The hub state machine [portable] and the hub task around it
    hub_auth/               PIN and token hashing, rate limits        [portable]
    http_server/            Ports 11915 and 80, WebSocket glue, static files
    setup_api/              The setup network's HTTP API (section 9.3)
    net/                    Wi-Fi state machine, setup network, captive DNS, mDNS, SNTP
    settings/               Typed access to saved settings (NVS)
    board/                  Per-chip pin tables and defaults (section 4.4)
    ota/                    Firmware update and rollback
    button/                 BOOT button hold detection
    status_led/             LED patterns
    web_assets/             Generated from the built web pages; never edited
  test/
    host/                   Host tests for the [portable] components, with a simulated DSPi
  tools/
    build.sh                Builds each chip in Espressif's Docker image
    qemu_smoke.sh           Boots a built image twice in QEMU
    gen_policy.py           policy/commands.json -> link_policy C table (committed)
    embed_web.py            js/apps/web/dist -> web_assets C source at build time
    link_smoke.mjs          Drives a live bridge end to end with dspi-link-js
```

Components marked `[portable]` include no ESP-IDF headers. They take time,
randomness and I/O through small interfaces passed in by their caller, so
the same source compiles into the firmware and into the host tests.

### 6.4 Components

| Component | Responsibility | Depends on |
|-----------|----------------|------------|
| `bridge_config` | Named constants: timeouts, queue depths, limits, retry counts. | nothing |
| `link_codec` | Encodes and decodes Link binary frames (Link spec 8), and base64 for the `_b64` fields. | nothing |
| `link_policy` | `link_policy_class(bRequest, dir)`, generated from `policy/commands.json` with the spec version it came from. Unknown codes are `config`. | nothing |
| `dspi_frame` | Builds DSPi UART requests and parses responses and notifications byte by byte (firmware spec 3). | nothing |
| `dspi_uart` | The link engine (`dspi_link.c`, portable): one request at a time, the idle gap, retries, recovery after timeouts, baud detection and the device's identity. The uart task (`dspi_uart.c`) runs it on the real UART and reports the device found or lost. | `dspi_frame` |
| `hub` | The hub state machine (portable): sessions, hello, auth, inventory, command routing, polls, notify relay, snapshot, locks, administration, the hub's own update, with JSON through cJSON. The hub task (`hub_task.c`) runs it and connects it to the HTTP server, the uart task, the ota task, flash and mDNS. | `link_codec`, `link_policy`, `hub_auth`, cJSON |
| `hub_auth` | PIN hashing and checking, token creation and hashing, rate limits. | mbedtls (behind an interface) |
| `http_server` | Starts both HTTP servers, upgrades WebSockets, serves static files, and hands frames and setup requests to its caller through callbacks, so it depends on neither the hub nor the setup API. | `web_assets`, `net` |
| `setup_api` | The setup network's endpoints, answering only on the setup network. | `net`, `settings`, `hub`, `hub_auth`, `dspi_uart`, `board` |
| `net` | Wi-Fi states (section 9.1), setup network, captive DNS, mDNS records, SNTP. | `settings` |
| `settings` | Reads and writes every saved value through typed functions, with defaults and a schema version. | `nvs_flash` |
| `board` | The pin table for the chip the image was built for, and PSRAM detection. | `soc` |
| `ota` | Writes an image in chunks on its own task, verifies it, switches partitions and confirms a new image after a healthy start. | `app_update` |
| `button` | Times how long BOOT is held (`button_machine.c`, portable) and samples the pin on a timer. | `driver/gpio` |
| `status_led` | Chooses the pattern of section 13 (`led_pattern.c`, portable) and drives a plain LED through LEDC or a WS2812. | `led_strip` |

### 6.5 Boot sequence

`app_main` does only this, in this order, with one line of comment per
step:

1. Start the button watcher, first, so a factory reset works even when a
   later step fails (section 12.1).
2. Initialise NVS. If it reports a version mismatch or no free pages, erase
   and initialise it again, and log that this happened.
3. Load settings (`settings`). On first boot, generate the hub id (UUID v4
   from the hardware random number generator) and default name. Log the
   startup summary, including what started the bridge (section 12.3).
4. Start the status LED.
5. Start networking (`net`): station mode if a home network is saved,
   otherwise the setup network. mDNS starts here, so the hub has somewhere
   to advertise from its first moment.
6. Start the hub task, which starts the uart task with the saved pins and
   baud rate.
7. Start the HTTP servers, with Link sessions going to the hub task and the
   setup network's requests to the setup API. `app_main` connects the two so
   that neither component depends on the other.
8. Start the supervisor (section 12.3).
9. If this image is new, start the rollback confirmation timer (section 14.2).

If a step fails, the bridge logs which step it was, waits 60 s with the
button still watched, then restarts. It does not abort at once, because a
restart loop shorter than a second would leave no time to hold BOOT. An
image on trial then returns to the previous one.

---

## 7. The DSPi UART link

### 7.1 Framing

The bridge uses the DSPi's UART framing exactly as the firmware spec
defines it. In summary:

| Frame | Bytes |
|-------|-------|
| SET request | `A5 01 bReq wValL wValH wIdxL wIdxH wLenL wLenH payload crcL crcH` |
| GET request | `A5 02 bReq wValL wValH wIdxL wIdxH wLenL wLenH crcL crcH` |
| SET response | `A5 81 status 00 00 crcL crcH` |
| GET response | `A5 82 status lenL lenH payload crcL crcH` |
| Notification | `A5 40 00 lenL lenH packet crcL crcH` |

The CRC is CRC-16/CCITT-FALSE (polynomial 0x1021, initial value 0xFFFF, no
reflection, no final XOR) over every byte after the `A5`, sent
little-endian. The firmware spec's worked example
`A5 01 D2 00 00 00 00 04 00 00 00 A0 C1 CF 6D` is a host test.

A Link CMD maps onto one UART request: `dir = 0` becomes a SET frame and
`dir = 1` a GET frame, with `bRequest`, `wValue`, `wIndex` and `wLength`
copied unchanged. A GET response's status byte becomes the CMD response
status unchanged (Link spec 8.1).

Responses carry no request id, so the link depends on strict
one-at-a-time use.

### 7.2 Timing

- **One request at a time.** The uart task sends the next request only after
  the previous one has been answered or has timed out.
- **Idle gap.** After every response, the uart task waits at least 2 ms
  before sending again, reading any notification frames that arrive in the
  gap. Without this, a busy stream of requests starves notifications (Link
  spec 9.2).
- **Continuous reading.** The UART receive path is always running. A
  notification frame that arrives while a response is awaited is relayed and
  does not disturb the wait.
- **Timeouts.** The wait for a response is 150 ms plus twice the wire time of
  the largest possible response at the current baud rate. At 1 Mbaud that is
  about 160 ms for an ordinary command and about 280 ms for a bulk read. The
  150 ms margin covers the DSPi's flash-write blackout of about 45 ms and
  its internal 50 ms busy retry. Bytes that arrived before the deadline
  still count when they are parsed after it, so a briefly delayed uart task
  does not turn a real answer into a timeout.
- **Resynchronising.** A frame that fails its CRC, claims more payload than
  a frame can carry, or stops arriving for 20 ms is discarded, and its bytes
  after the `A5` are searched again for a sync byte. Noise or a lost byte
  therefore costs at most the damaged frame, never the next one.

### 7.3 Retries

The uart task sends a request at most twice, and only in these cases:

- **The DSPi answers `CRC_ERROR` (0x05) or `FRAME_ERROR` (0x07).** The DSPi
  rejected the frame itself and did not run the request, so any request,
  SET included, is sent again.
- **No valid answer arrives** (a timeout, or an answer that fails its CRC),
  and the request is a GET that `link_policy` classes as *read*. Such a
  request is safe to repeat. Write-as-read commands such as
  `REQ_RTA_CONTROL` (0x0E) and `REQ_CS_MACRO_FIRE` (0x25) are excluded,
  because their GET frames change state.

Other requests with no valid answer are not repeated, because the bridge
cannot know whether they ran. `BUSY` (0x01) and `BULK_LOCKED` (0x04) are
passed to the client unchanged, since the firmware has already retried for
50 ms. When the last attempt gets no valid answer, whether nothing came or
the answer failed its CRC, the client receives Link status `0x82 TIMEOUT`:
the command may or may not have run. `0x05 CRC_ERROR` reaches a client only
when the DSPi itself reports it, meaning the DSPi did not run the frame, so
a client that retries on it never repeats a command that ran. A damaged
frame counts as an answer only if its header says it is the kind awaited; a
damaged notification followed by silence is simply no answer. Every DSPi
status is passed on unchanged.

### 7.4 Recovery after a timeout

A response that arrives late could be mistaken for the answer to the next
request, because responses carry no request id. Two rules prevent this:

- **Quarantine.** After a request gets no valid answer, the uart task
  discards every incoming frame except notifications until the line has
  been silent for 20 ms (at most 500 ms in all), and only then sends again.
- **Shape.** An answer counts only if its type matches the request (SET or
  GET response) and, for a GET, it is no longer than the `wLength` asked for,
  since the firmware never returns more. Anything else is a late answer and
  is dropped.

After 3 requests in a row without a valid answer, whether they timed out
or got a damaged answer, the uart task also reinstalls the UART driver,
which clears a receiver stuck on noise. Damaged answers count as well,
because noise is exactly what a stuck receiver produces. This applies only
while a device is present: probes (section 7.5) do not count, because each
probe changes the baud rate, which already flushes the receiver.

### 7.5 Finding the baud rate

When automatic detection is on (`uart.detect_baud`, section 8.11), the uart
task looks for the DSPi by sending `REQ_GET_CTRL_IFACE_STATUS` (0xF9, 8
bytes) at each rate in turn, starting with the saved rate and then trying
1,000,000, 921,600, 460,800, 230,400 and 115,200. The first rate that
returns a frame with a valid CRC wins and is saved. When detection is off,
only `uart.baud` is tried.

### 7.6 Device presence

The uart task reports the device as found or lost; the hub turns those
reports into Link events.

- **Finding.** While no device is known, the uart task probes every 2 s
  (section 7.5). On an answer it reads, in order, `REQ_GET_SERIAL` (0x7E),
  `REQ_GET_PLATFORM` (0x7F, 7 bytes), `REQ_GET_CTRL_IFACE_STATUS` (0xF9) and
  `REQ_GET_UART_CONFIG` (0xF6), then reports the device found with those
  values. The hub assigns handle 0 and sends `device.added`.
- **Watching.** When the link has carried no traffic for 1 s, the uart task
  sends `REQ_GET_SERIAL` (0x7E) as a heartbeat. An answer with a different
  serial means another DSPi was plugged in: the uart task reports the device
  lost, and the next probe finds the new one (see Swapping). A quick reboot
  of the same DSPi cannot be told apart from no change.
- **Notifications** are relayed only while a device is present. Frames that
  arrive before the device is reported found are dropped, since no client
  could yet place them.
- **Losing.** Three requests in a row without a valid answer mark the
  device lost. A single timeout never does (Link spec 8.2). The hub reports the
  device `offline` and keeps handle 0 for 60 s, then sends
  `device.removed`.
- **Swapping.** If a device returns with a different serial, the hub sends
  `device.removed` for the old one and `device.added` for the new one.

### 7.7 Sharing the link

Client commands, polls and the bridge's own reads all go through the uart
task's single queue, in this order of priority:

1. client commands, in arrival order across all sessions (Link spec 8.2);
2. poll ticks that are due;
3. the bridge's own reads (heartbeat, snapshot refresh).

Polls are granted against a budget of `baud / 25` bytes per second (40,000
at 1 Mbaud), which leaves more than half of the link for commands and
notifications. The budget is shared by every session, not given to each.
Identical polls from different sessions are read once, at the fastest rate
asked for, and count once. A `poll.subscribe` whose polls cannot all be
granted at least 1 Hz within what is left is answered `busy`, and nothing
changes.

---

## 8. Hub behaviour

### 8.1 hello

```json
{"t": "hello",
 "proto": {"major": 1, "minor": 2},
 "hub": {"id": "<uuid>", "name": "DSPi Bridge 3F2A", "kind": "bridge", "version": "1.0.0"},
 "auth": "none",
 "caps": ["cmd", "notify", "poll", "snapshot", "lock", "web", "rename",
          "hub_config", "wifi", "auth_config", "reset", "hub_update"],
 "limits": {"max_frame": 16384, "max_payload": 8192, "max_inflight": 4,
            "poll_max_hz": 20, "poll_budget_bps": 40000}}
```

- `version` is the bridge firmware's version.
- `notify` is omitted when the DSPi's `notify_enable` is off (section 5).
- `pin_source` is `"fixed"` whenever `auth` is `"pin"`.
- `poll_budget_bps` follows the current baud rate (section 7.7).

### 8.2 Sessions

- The bridge accepts up to 4 sessions, or 8 on a board with PSRAM. Beyond
  that it answers the WebSocket upgrade with HTTP 503. A closed session
  keeps its place for the few tens of milliseconds the hub takes to see it
  close (section 6.2), so a client that reconnects at once may briefly get
  503; Link's reconnect backoff covers this.
- An upgrade that does not ask for the `dspi-link-1` sub-protocol is
  refused with HTTP 400 (Link spec 4). One whose `Origin` names any site
  but the bridge, by the address the connection arrived on or by
  `<hostname>.local`, is refused with HTTP 403.
- Each port is served by one task, so each HTTP request or WebSocket frame
  has 8 s from its first byte to arrive, and each reply 8 s to leave; a
  client that trickles one past that is dropped, alone. When every socket
  on the Link port is taken, a new connection replaces the one used least
  recently, and TCP keep-alive drops peers that vanished without closing,
  so idle connections cannot hold the port.
- With the PIN on, one address may hold at most 2 sessions that have not
  yet authenticated. A third closes that address's oldest unauthenticated
  session with code 4001, so one client cannot fill every slot before
  pairing.
- JSON messages, on the Link port and from the setup page, may nest at
  most 8 levels; anything deeper is refused unparsed, because the parser's
  recursion would otherwise overflow the stack of the task parsing it.
- Keepalive and close codes follow Link spec 4 and 5.
- Each session has an outbound queue of 32 frames. One job on the HTTP
  server's task sends what waits: each session's frames encoded into one
  batch, written without waiting, so a socket takes what fits and the rest
  goes on the next pass. Batching keeps fast meter polls from flooding the
  TCP stack with tiny writes, and never waiting means a client that reads
  slowly, or sits on a weak Wi-Fi link, falls behind on its own without
  holding up anyone else.
- When a session's queue is full, the hub drops notifications and poll
  frames for it and, once there is room again, sends RESYNC with reason 0.
  A session that has sent nothing for 10 s while frames wait is closed with
  code 1011.
- Pongs and close replies join the same queue, since writing them straight
  to the socket could split a batch that is part sent.
- A session's send queue is also capped at 32 KB, so a client that asks for
  bulk reads and never reads them cannot take the bridge's memory.
- A frame too large, or fragmented (a first fragment included), is read and
  thrown away, up to 64 KB, so the close code 4000 that follows reaches the
  client; anything larger is cut off at once.
- The Link WebSocket also answers on the setup network, so a phone on the
  setup network can control the DSPi before any home network is set up.

### 8.3 Commands

The hub checks each CMD frame in this order, answering with the first status
that applies:

1. `BAD_FRAME` if the frame is malformed.
2. `NO_DEVICE` if handle is not 0 or the device is offline.
3. `TOO_LARGE` if the payload exceeds `max_payload`.
4. `DENIED` if the session's role does not permit the command's class.
5. `LOCKED` if another session holds the lock.
6. `RATE_LIMITED` if the session already has `max_inflight` commands queued.
7. `BUSY` (0x01) if a firmware update is in progress and the command is a
   GET whose `wLength` exceeds 2048 bytes, half the UART's receive buffer
   (section 14.2). The command has not run, so the client may retry.

Then the command joins the uart task's queue and its response returns to the
session with the client's `tag`. The hub never interprets payloads.

### 8.4 Notifications

Every notification frame from the DSPi is relayed to every authenticated
session as a NOTIFY frame (Link spec 8.3). For `origin`, the hub keeps a
queue of the sessions that sent writes, in dispatch order: SETs, and GETs
that `link_policy` classes as anything but *read* (write-as-read commands
such as `REQ_RTA_CONTROL`). An entry's 1 s lifetime counts from when the
uart task takes the write up, not from when it was queued, and a write the
DSPi refuses leaves the queue at once, since it will cause no notification.
A notification whose source byte is `HOST_SET`, `BULK_SET` or `UART` takes
the oldest entry.

The hub also watches the notification `seq` byte. On a gap it sends RESYNC
with reason 0 to every session and marks the snapshot cache stale.

### 8.5 Polls

Poll subscriptions follow Link spec 7.6. Identical polls (same `req`, `val`,
`idx` and `len`) from several sessions run once, at the fastest rate any of
them was granted, and fan out. Each session still receives answers only at
its own granted rate. The hub stops polling as soon as no session
subscribes.

### 8.6 Snapshot

The hub keeps one cached bulk blob and refreshes it by re-reading, as Link
spec 7.5 allows, rather than by patching. Any `PARAM_CHANGED` or
`BULK_INVALIDATED` notification, and any write the hub forwards (a SET or a
write-as-read GET), marks the cache stale. When the DSPi has
`notify_enable` off, no notification can say what changed, so every
snapshot re-reads the blob.

Every `device.snapshot` request gets a fresh status read, plus a blob read
when the cache is stale, which takes about 61 ms at 1 Mbaud. A request that
arrives while a status read is already under way waits for the next one,
so its answer never predates it. The hub matches each read's answer to the
read by its tag, so an answer that arrives after the device went offline
and came back is ignored. The blob is also read once when the device is
found, which gives `wire_version` for the device object.

### 8.7 Locks, names and stats

- Locks follow Link spec 7.7.
- Device names (`device.rename`) are saved by serial, for up to 8 devices.
  The default name is `DSPi` followed by the last 4 characters of the
  serial.
- `hub.stats` adds a `bridge` object:

```json
"bridge": {
  "chip": "esp32s3", "psram": true, "heap_free": 182340, "uptime_s": 8812,
  "wifi": {"ssid": "Home", "rssi": -50, "ip": "192.168.1.40", "mac": "a0:b7:65:12:3f:2a"},
  "uart": {"baud": 1000000, "tx_pin": 17, "rx_pin": 18, "live": true,
           "notify_enable": true, "requests": 52113, "timeouts": 0, "crc_errors": 0,
           "retries": 0}
}
```

### 8.8 Discovery

- mDNS hostname: the `hostname` setting, by default `dspi-bridge-xxxx`.
- DNS-SD: one `_dspi._tcp` instance on port 11915, named after the hub, with
  the TXT keys of Link spec 3.1 (`kind=bridge`, `web=1`). The record is
  updated whenever the name, auth mode or device list changes.
- `/dspi/v1/info` as Link spec 3.2.
- Port 80 serves the setup page on the setup network and, on the home
  network, a redirect to `http://<host>:11915/`.

### 8.9 Authentication

- The default is `auth: none`.
- With the PIN on, the bridge is in `pin` mode with `pin_source: "fixed"`.
  Every paired client is granted `admin`, whatever role it asks for.
- The PIN is stored as PBKDF2-HMAC-SHA256 with a 16-byte random salt and
  1,000 iterations. The hash keeps the PIN from being read straight out of
  a copy of the flash; with only a million possible PINs, no iteration count
  would stop an offline search, so the rate limits are the real defence.
  The count is kept low because each check runs on the hub task: 10,000
  iterations took 1.5 s on an ESP32-S3 and held up every session.
- Tokens are 32 bytes from the hardware random number generator. The bridge
  stores their SHA-256 hashes for up to 16 clients. When the list is full,
  pairing a new client evicts the one seen least recently, and any session
  the evicted client still has open is closed with code 4002.
- `last_seen` is refreshed every minute while a client is connected, and
  saved at most once an hour per client, to spare flash. Timestamps come
  from SNTP. A timestamp taken before the clock was set is left out of
  `auth.list`.
- Client names are stored and sent as valid UTF-8: each bad byte or control
  character becomes `?`, and a long name is cut only between characters.
  Names saved by older firmware are repaired when loaded.
- `hub.stats` and `hub.rename` require `admin` (Link spec 7.9), and
  `device.rename` requires `control` or `admin`.
- If the saved PIN data cannot be read, the bridge fails closed: it reports
  `auth: "pin"`, answers every `auth.pair` and `auth.token` with `internal`,
  and recovers when a new PIN is set, from the setup page after a factory
  reset (section 12.2).
- `auth.list` and `auth.revoke` are supported. `auth.set_role` answers
  `unsupported`, because every paired client is `admin`.
- Rate limits follow Link spec 6.2 for a fixed PIN.
- The settings page's browser is an ordinary Link client: it pairs with the
  PIN once and keeps its token.

### 8.10 Administration

The bridge implements Link spec 7.10 to 7.14:

- `hub.wifi.join` keeps the previous network's credentials until the new
  network has given the bridge an address.
- `hub.reset` with `network` forgets the home network and restarts into the
  setup network. With `factory` it erases every NVS namespace the bridge
  owns and restarts into first-time setup with a new hub id.
- `hub.update` is described in section 14.
- A change the bridge cannot save or apply (`hub.config.set`, `hub.rename`,
  `device.rename`) is answered `internal`, and the settings stay as they
  were. `hub.wifi.scan` is answered `busy` while another scan runs, and
  `internal` if a scan cannot start. `auth.pair`, `auth.token` and
  `hub.auth.set` are answered `internal` if the bridge's cryptography fails,
  and such a failure never counts against the client or changes the PIN.
- `device.snapshot` keeps one waiting request per session; a second one
  while the first waits is answered `busy`.

### 8.11 Configuration keys

`hub.config.get` returns:

```json
"config": {
  "name": "DSPi Bridge 3F2A",
  "hostname": "dspi-bridge-3f2a",
  "uart": {"tx_pin": 17, "rx_pin": 18, "baud": 1000000, "detect_baud": true},
  "led":  {"type": "none", "pin": null}
},
"options": {
  "uart": {"tx_pin": [1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42],
           "rx_pin": [1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42],
           "baud": [1000000, 921600, 460800, 230400, 115200]},
  "led":  {"type": ["none", "gpio", "gpio_active_low", "ws2812"],
           "pin":  [1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 48]}
}
```

| Key | Meaning | Applied |
|-----|---------|---------|
| `name` | The hub's name in DNS-SD and `hello`. 1 to 63 bytes of UTF-8. | At once |
| `hostname` | The `.local` hostname. 1 to 63 characters: lower-case letters, digits and hyphens, not starting or ending with a hyphen. | At once (mDNS re-registers) |
| `uart.tx_pin`, `uart.rx_pin` | The ESP32 pins wired to the DSPi. Must differ and must not be the LED pin. | At once (UART reinstalled) |
| `uart.baud` | The rate in use. With detection on, the bridge overwrites it with the rate it finds. | At once |
| `uart.detect_baud` | Search the rates in section 7.5 when the DSPi does not answer. | At once |
| `led.type` | `none`, `gpio` (on when high), `gpio_active_low` or `ws2812` (one addressable LED). | At once |
| `led.pin` | The LED's pin, or `null` when `led.type` is `none`. | At once |

Changing the UART pins or baud rate drops the device (`offline`) until it
is found again, which the settings page explains before saving. When one
change moves both the UART and the LED, the LED lets go of its old pin
first, the UART moves, and the LED starts last, so the two may trade pins in
a single change.

---

## 9. Networking

### 9.1 Wi-Fi states

| State | Radio | Enters when | Leaves when |
|-------|-------|-------------|-------------|
| `SETUP` | Setup network only | No home network is saved. | The setup page saves one: `CONNECTING`. |
| `CONNECTING` | Station, plus the setup network if it was already up | A home network is saved (boot, setup, `hub.wifi.join`). | Connected: `ONLINE`. Not connected within 3 minutes: `FALLBACK`. |
| `ONLINE` | Station only | The station has an IP address. | Disconnected: `CONNECTING`, and after 3 minutes without a connection, `FALLBACK`. |
| `FALLBACK` | Station and setup network | A saved home network has been unreachable for 3 minutes. | Connected: `ONLINE`, with the setup network kept up for a while (below). |

Once `ONLINE`, a setup network that is still up goes off at the first of:
5 s after the setup page sends `finish`, 2 minutes after the join, or 60 s
with no phone or computer on it.

A new network's credentials are saved only once the bridge has an address
on it. A join fails when the password is refused twice in a row (one
refusal can be a handshake that merely timed out), or when 30 s pass
without an address; the bridge then returns to the network it had, if any.

In `CONNECTING` and `FALLBACK` the station retries with growing delays (5 s,
10 s, 20 s, then every 30 s). A once-a-second check also schedules a retry
if an attempt has reported nothing for 20 s, so a lost driver event can
never leave the station idle. The setup network must share the station's
channel, so its channel may change while the station searches; phones
rejoin by themselves.

Every ESP32 in section 4.1 supports only 2.4 GHz Wi-Fi. The setup page says
so above the network list.

### 9.2 The setup network

- Name `DSPi Bridge XXXX`, open (no password), address 192.168.4.1, with
  DHCP for up to 4 phones or computers.
- A captive DNS server answers every name with 192.168.4.1, and the HTTP
  server answers the operating systems' connectivity checks
  (`/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`,
  `/ncsi.txt` and similar) with a redirect to the setup page. This makes
  the setup page open by itself on phones.
- The setup page and every asset it uses are embedded in the firmware. The
  setup network has no internet, so nothing may load from elsewhere.
- When the PIN is on (only possible in `FALLBACK`), every setup API call
  that changes something must carry it.

The setup network is open because nothing else is practical on a device
without a screen, and it is only up while the bridge has no working home
network. A user who wants that window protected turns the PIN on.

### 9.3 The setup API

Plain HTTP and JSON under `/setup/v1/`, answered only on the setup network:
a request counts only if it arrived on the setup network's address from an
address on its subnet, and from the home network port 80 only ever
redirects to the Link port. The API also answers only requests whose
`Host` is `192.168.4.1`, and takes POSTs only as `Content-Type:
application/json`, so a web page open in the phone's browser cannot drive
it. These paths are outside the Link protocol (Link spec 4.2) and carry no
device commands.

```
GET  /setup/v1/state
200  {"name": "DSPi Bridge 3F2A", "chip": "esp32s3", "version": "1.0.0",
      "pin_required": false,
      "dspi": {"found": true, "serial": "E46058388B1A2E2C", "baud": 1000000, "notify_enable": true},
      "wifi": {"saved_ssid": null, "join": "idle", "ip": null, "hostname": "dspi-bridge-3f2a",
               "setup_ssid": "DSPi Bridge 3F2A"}}

GET  /setup/v1/networks
200  {"networks": [{"ssid": "Home", "rssi": -52, "secure": true}]}

POST /setup/v1/apply
     {"ssid": "Home", "password": "<passphrase>", "name": "Living Room DSPi",
      "pin_enabled": false, "pin": null, "current_pin": null}
202  {}

POST /setup/v1/finish
     {"current_pin": null}
200  {}
```

- `wifi.join` in `state` is `idle`, `connecting`, `connected` or `failed`;
  when `failed`, `wifi.failure` says why: `wrong_password`, `not_found` or
  `timed_out`. The setup page polls `state` once a second after `apply`.
- `apply` saves the name and PIN choice and starts `CONNECTING`. The setup
  network stays up so the page can show the result.
- `finish` turns the setup network off, 5 s later. If the user never sends
  it, the setup network goes off 2 minutes after the join succeeds.
- `pin_enabled` true sets `pin`, six digits; false turns the PIN off; left
  out, the PIN stays as it is, which is how the page keeps an existing PIN.
- `current_pin` is required by `apply` and `finish` when the bridge has a
  PIN. After an `apply` that changed the PIN, `finish` takes the new one.
- Errors come with `{"error": "<code>", "msg": "<plain sentence>"}`. The
  codes are `bad_request` (400), `pin_required` and `wrong_pin` (403),
  `rate_limited` (429), and `busy` (503, when the bridge could not carry out
  the request just then; the page may try again).
- A password is empty for an open network, or 8 to 63 characters, or
  exactly 64 hex digits, as WPA2 allows.
- `networks` answers `busy` when a scan fails or another scan holds the
  radio, rather than an empty list.

---

## 10. Web pages

### 10.1 Pages

| Page | Where | Built from |
|------|-------|-----------|
| Setup page | `/` on the setup network, port 80 | `js/apps/web/setup.html`, a second entry point in the existing web app |
| Control page | `/` on port 11915 | `js/apps/web/index.html`, the existing DSPi web client |
| Settings page | A screen inside the control page | New screens in `js/apps/web`, shown when the hub has `hub_config` |

Keeping all three in one Vite app gives them one look, one set of components
and one build. The settings screens use only Link messages, so they would
work unchanged against any future hub with the same capabilities.

### 10.2 Setup page

Five short steps, one main button each.

1. **Welcome.** "Let's connect your DSPi Bridge to your Wi-Fi." Below it, the
   DSPi status: a green "DSPi found" with the serial, or an amber "DSPi not
   found yet" with a link to the wiring diagram. Setup continues either way.
2. **Choose your network.** Nearby networks, strongest first, with signal
   bars and a lock for secured ones, a Refresh button and "Enter a network
   name". A note: "Only 2.4 GHz networks are shown." Choosing a secured
   network asks for its password, with a show/hide control.
3. **Name and security.** The bridge's name, filled in with
   `DSPi Bridge XXXX`. Then one switch, off by default:

   > **Protect this bridge with a PIN**
   >
   > When this is on, anyone who opens these pages or connects from DSPi
   > Console must enter the PIN once on each device. When it's off, anyone on
   > your network can control your DSPi and change these settings.

   Turning it on reveals a 6-digit PIN field, a confirmation field, and:

   > You'll need this PIN the first time you connect from each browser,
   > computer or phone. If you forget it, hold the BOOT button for 15
   > seconds to factory reset the bridge.

4. **Connecting.** A progress indicator while `wifi.join` is `connecting`.
   On `failed`: "Couldn't join Home. Check the password and try again.", back
   to step 2 with the network still selected.
5. **Done.** "Your bridge is on Home." The address
   `http://dspi-bridge-3f2a.local:11915` as a large copyable link, the IP
   address as a fallback, and the reminder "Reconnect this phone to Home to
   open it." The button "Finish" sends `finish`.

### 10.3 Settings page

Sections in one scrolling screen, each with a heading and plain sentences:

- **Status.** DSPi connection (found, serial, firmware, link health), Wi-Fi
  network and signal, bridge firmware version and uptime.
- **DSPi connection.** TX and RX pin pickers fed by `options`, a wiring
  diagram that redraws with the chosen pins, the baud rate with "Detect
  automatically", and the warnings from section 5.
- **Wi-Fi.** The current network and "Change network", which scans, takes a
  password and explains that the page will reconnect when the bridge has
  moved.
- **Security.** The PIN switch with the same wording as the setup page, and
  "Change PIN". When the PIN is on, the paired browsers and apps, with when
  each was last seen and a Remove button.
- **Bridge.** Name, hostname and status LED.
- **Maintenance.** "Update firmware" (choose a file, see progress), "Restart",
  "Forget Wi-Fi" and "Factory reset". The last two ask for confirmation in a
  dialog that says exactly what will be erased.

### 10.4 Design rules

- Mobile first. Every screen works at 360 px wide and scales up to a centred
  column on large screens.
- Light and dark themes following the system setting, with colours defined
  once as CSS custom properties.
- The system font stack; no web fonts, because the setup network has no
  internet and every byte is stored in flash.
- Plain language. No jargon on screen without a one-line explanation:
  "baud rate" appears with "the speed of the link to your DSPi".
- One main action per screen, visually distinct from the rest. Destructive
  actions are red and always confirmed.
- Accessible: real labels on every control, visible keyboard focus, WCAG AA
  contrast, and status never shown by colour alone.
- No UI framework, as in the existing web app. Everything served must fit in
  250 KB gzipped.

### 10.5 Build and embedding

`npm run build` in `js/` produces `js/apps/web/dist/`. The firmware build
runs `tools/embed_web.py`, which gzips every file and writes them into the
`web_assets` component as C arrays with their paths, content types and
SHA-256 ETags. If `dist/` is missing, the firmware build stops with a
message giving the `npm` command to run. Release CI always builds the web
pages first.

---

## 11. Saved settings

All values live in NVS, in namespaces the bridge owns. `settings` is the only
module that touches NVS, and every value has a typed getter, a setter and a
default.

| Namespace | Keys | Erased by |
|-----------|------|-----------|
| `bridge` | `schema` (u8), `hid`, `name`, `hostname`, `uart_tx`, `uart_rx`, `uart_baud`, `uart_detect`, `led_type`, `led_pin` | factory reset |
| `wifi` | `network` (one blob: the name and password, always written together; the older separate `ssid` and `password` keys are read only when it is missing, and erased at the next save) | network or factory reset |
| `auth` | `store` (one blob: a layout number, then whether the PIN is on, its hash and salt, and up to 16 records of token hash, name, created and last seen) | factory reset |
| `devices` | `names` (blob: up to 8 records of serial and name) | factory reset |

- The bridge stores Wi-Fi credentials itself and keeps ESP-IDF's own copy in
  RAM only (`WIFI_STORAGE_RAM`), so forgetting a network is one explicit
  erase.
- If the `auth` blob is saved but cannot be read, or has a layout this
  firmware does not know, the bridge starts with the PIN on and no usable
  PIN, refusing every pairing, rather than starting open. A factory reset
  recovers it. A firmware that changes the layout migrates the old one.
- Every key is written before `schema`, so a power cut part way through
  leaves no schema and the next start begins again from defaults. A key
  that is missing or invalid at start is saved back with its default, so a
  generated value such as the hub id is never generated twice.
- `schema` starts at 1. A firmware that finds an older schema migrates it
  step by step; one that finds a newer schema (after a downgrade) erases
  only the keys in `bridge` it does not know, rewrites the ones it knows
  with `schema` last, and keeps the hub id and the other namespaces.
- Nothing is written on the command path. Settings are written only when the
  user changes them, plus the hourly `last_seen` update.

---

## 12. Reset and recovery

### 12.1 BOOT button

The BOOT button is read from the first step of the boot (section 6.5), so
it works even when a later step fails. Holding it while power is applied
puts the ESP32 into its download mode instead, which the README mentions.

| Hold | LED (if fitted) | On release |
|------|-----------------|------------|
| under 5 s | no change | Nothing. |
| 5 to 15 s | fast amber blinking | **Forget Wi-Fi**: erase the home network and restart into the setup network. Settings, PIN and paired clients are kept. |
| 15 to 30 s | solid red | **Factory reset**: erase everything (section 11) and restart into first-time setup. |
| over 30 s | off | Nothing: the hold is cancelled. |

The action happens on release, and the LED always shows which action a
release would trigger at that moment. Holding for more than 30 s cancels
both, for a user who changes their mind. The settings page's Maintenance
section offers both actions too. A release normally goes through the hub,
which closes every session with code 4003 first; if the hub cannot take it
at once, the button erases and restarts by itself, so a reset works even
when the rest of the firmware is stuck.

The pin is read every 20 ms, and a press or release counts only after three
readings in a row agree, which hides contact bounce. The button counts only
after it has been seen released once, so a pin that reads pressed from the
start, on a board without the button, never triggers anything. Each stage
is also written to the serial log, for boards without an LED.

### 12.2 Forgotten PIN

A forgotten PIN is fixed with a factory reset. There is deliberately no
other way, because any other way would also be open to a stranger.

### 12.3 Crashes

The bridge restarts itself on a crash or a task watchdog timeout
(`CONFIG_ESP_TASK_WDT_PANIC`), and logs what started it as "Started by: …"
in the startup summary. A supervisor timer checks every 5 s that the hub
task and the uart task are still going round their loops, and restarts the
bridge when either has stopped for 30 s. Section 14.2 handles a new
firmware that keeps crashing.

---

## 13. Status LED

Off by default (section 8.11). When fitted:

| Pattern | Meaning |
|---------|---------|
| Slow blue pulse | `SETUP` or `FALLBACK`: join the setup network. |
| Fast blue blink | `CONNECTING`. |
| Solid green for 5 s, then off | `ONLINE` with the DSPi found. |
| Slow amber blink | `ONLINE`, but the DSPi is not answering. |
| Fast amber blink | BOOT held 5 to 15 s. |
| Solid red | BOOT held 15 to 30 s. |

A single-colour LED shows the same timing in its one colour, at the
brightness of the colour's brightest part. Fast blinks run 4 times a
second; slow blinks and pulses take 2 s. The LED is redrawn 50 times a
second, a plain LED through LEDC so that a pulse can fade, and a WS2812 is
dimmed to about a fifth of its full brightness, which is glaring.

---

## 14. Firmware updates

### 14.1 Partitions

The same 4 MB table serves every chip:

| Name | Type | Size |
|------|------|------|
| `nvs` | data, nvs | 24 KB |
| `otadata` | data, ota | 8 KB |
| `phy_init` | data, phy | 4 KB |
| `ota_0` | app | 1920 KB |
| `ota_1` | app | 1920 KB |

The firmware image, with the web pages embedded, must stay under 1.5 MB so
that the margin covers future growth. CI fails a build that exceeds it.

### 14.2 Updating and rollback

- The settings page uploads a firmware file through `hub.update` (Link spec
  7.14), as FWDATA chunks of `max_frame - 8` bytes (16,376) written straight
  into the inactive partition.
- The ota task writes the chunks, away from the hub task, which keeps
  serving sessions meanwhile. It holds at most 2 chunks waiting to be
  written, and the hub acknowledges each chunk only once it is in flash, so
  a client keeps at most 2 chunks unanswered and never sends faster than
  the flash takes them. An update with no progress for 30 s is abandoned.
- The bridge checks the SHA-256 and lets ESP-IDF check the image, which
  rejects a file that is not firmware or was built for another chip. It
  then checks that the image is bridge firmware (its project name), so a
  wrong file cannot leave the board without the bridge.
- On success it switches the boot partition and restarts. Every refusal
  ends with `hub.update.done` carrying a plain sentence, and the bridge
  keeps running its current firmware.
- An update abandoned part way (its sender left, it stalled, it was
  refused) writes nothing more and is never switched to, even if its
  final check was under way. One whose every byte has arrived is finished
  even if its sender leaves during the check: a good image then starts,
  and the client sees the new version in `hello` when it reconnects.
- While a finished update waits for its restart, a new one is refused
  ("restart the bridge first"), so the image the bootloader will start is
  never overwritten.
- With rollback enabled in the bootloader, the new image starts in a
  pending state. It confirms itself once it runs properly: the hub task and
  the uart task both going round their loops, the bridge on its home
  network with an address or on its setup network, and the Link server
  running. The first check is at 60 s, then every 10 s; an image still not
  running properly after 5 min returns to the previous one. That limit is
  longer than the 3 min Wi-Fi fallback (section 9.1), so an image waiting
  for its home network is not judged before the setup network comes up. A
  confirmation that fails to write is tried again 10 s later. If the image
  crashes or restarts before it is confirmed, the bootloader returns to the
  previous image, except that a restart the user asks for (`hub.restart`,
  `hub.reset` or the BOOT button) first confirms an image that is running
  properly at that moment.
- While an update is written, flash erases pause the uart task for about
  45 ms at a time, and the UART's 4 KB receive buffer must hold whatever
  arrives meanwhile. So from `hub.update` until the update ends, the hub
  keeps large answers off the link: polls pause, `device.snapshot`
  requests wait and are answered once the update ends (or the bridge
  restarts into the new firmware), and large client GETs are answered
  `BUSY` (section 8.3). Small commands and notifications go on as usual.
- While a new image is still on trial, another update is refused ("try
  again in a minute"), because ESP-IDF keeps the image it would return to.
- On the ESP32-S3, a 983 KB image took about 4 to 7 s to send and check,
  and the bridge was back 7 s later.
- Updates are not signed, by design. Sending one takes an admin session,
  which can already control the DSPi and move the bridge to another
  network, and anyone with the board in hand can flash it over USB. A
  signature would add a release key to guard forever, and would stop
  people who build their own firmware from updating over the air. A PIN
  (section 8.9) is the protection for a shared network. Hardware secure
  boot and flash encryption are not used either: they burn eFuses that
  cannot be undone, and would lock the board to one key.

---

## 15. Distribution

- Each release is a GitHub release with, for every chip, a merged image for
  first installation (bootloader, partition table and app) and an app image
  for updates through the settings page. File names say the chip:
  `dspi-bridge-1.0.0-esp32s3.bin`, `dspi-bridge-1.0.0-esp32s3-update.bin`.
- The installer page uses ESP Web Tools, which flashes from Chrome or Edge
  over USB. Its manifest lists one build per chip family, so the page picks
  the right image by itself. The page is hosted on GitHub Pages from this
  repository, and serves ESP Web Tools from its own site: packaging
  downloads one pinned version, checks it against the registry's SHA-512
  integrity, and stops if it differs.
- A release is made by pushing a tag `bridge-v<version>`, where `<version>`
  is the firmware's `PROJECT_VER`. CI checks they match, builds every chip,
  boots each full image in QEMU up to starting the network (all chips but
  the ESP32-C6, which QEMU lacks; QEMU cannot start Wi-Fi), attaches the
  files and their SHA-256 sums to the release, and publishes the installer
  page. `tools/package_release.sh` does the packaging, locally as in CI.
  The workflows pin every action by commit, and the repository's
  `github-pages` environment needs a deployment rule for `bridge-v*` tags,
  or GitHub refuses to publish the page from a release tag.
- Developers build and flash with `idf.py` (section 17 and the README).

---

## 16. Code standards

The bridge is meant to be read and copied. These rules apply to every file,
C and TypeScript alike.

**Structure**

- One responsibility per component and per file. A file's name says what it
  does.
- Every piece of state has one owner (section 6.2). Other modules reach it
  only through the owner's functions. There are no globals shared between
  files.
- Components marked `[portable]` include no ESP-IDF headers and are covered
  by host tests.
- Generated files say so in their first line, name the script that made
  them, and are never edited by hand.

**Readability**

- Every file opens with a comment saying what it does, why it exists and
  where it fits, with the spec sections it implements ("Implements Link spec
  8.3 and bridge spec 8.4").
- Every public function is documented in its header: what it does, its
  parameters, what it returns, and who may call it (which task).
- Comments explain why, not what. A comment that restates the code is
  removed.
- Names are whole words, with a prefix for public symbols that names the
  component or its subject (`dspi_uart_send`, `hub_session_close`; the
  DSPi's frames use `dspi_`, Link's frames `link_`). Constants carry their
  unit (`UART_IDLE_GAP_MS`, `MAX_FRAME_BYTES`).
- Functions stay short (a guideline of 60 lines), return early on errors and
  avoid nesting deeper than three levels.
- Every tunable number is a named constant in `bridge_config`, with a
  comment giving its reason.

**Behaviour**

- No return value is ignored. Errors propagate as `esp_err_t` or a
  component's own enum, and every failure is either handled or logged with
  its component's tag.
- No heap allocation on the command or notification path. Buffers come from
  fixed pools sized by named constants. Allocation is allowed for rare work
  such as JSON replies and firmware updates, and is always checked.
- Every wait has a timeout. Nothing blocks forever.
- Log levels mean something: `E` for a fault that needs attention, `W` for
  something recovered, `I` for state changes, `D` for detail.

**Tooling**

- C17 with `-Wall -Wextra -Werror`, formatted by a committed
  `.clang-format`.
- TypeScript in strict mode, as in the existing workspace.
- Each component has a short `README.md` covering its job, its public API and
  its tests.

---

## 17. Testing

| Level | What | Where |
|-------|------|-------|
| Host unit tests | `link_codec` against every file in `fixtures/frames` and `fixtures/messages`; `dspi_frame` against the firmware spec's worked examples and corrupted, split and noisy byte streams; `link_policy` against `policy/commands.json`; `hub_auth` hashing and rate limits. | `bridge/esp32/test/host`, run in CI |
| Host link tests | The UART link engine against a simulated DSPi on a virtual clock: timeouts, late and corrupt answers, retries, the idle gap, notifications while waiting, baud detection and identity. | `bridge/esp32/test/host`, run in CI |
| Host hub tests | The `hub` state machine driven by scripted sessions and a simulated DSPi, with a fake clock: ordering, `max_inflight`, origin attribution, poll fan-out and stop, locks, offline and swap, RESYNC, auth modes and revocation. | `bridge/esp32/test/host`, run in CI |
| Build checks | Firmware for all four chips, image size under 1.5 MB, web build, generated files up to date. | CI |
| Boot in QEMU | `tools/qemu_smoke.sh` boots the ESP32, ESP32-S3 and ESP32-C3 images twice and checks that the settings survive and nothing crashes. | CI |
| Bench with a DSPi | A real bridge and DSPi on each of the four chips, driven by `tools/link_smoke.mjs` (the `dspi-link-js` client against the live bridge) and by DSPi Console. Walks the Link spec 12 conformance checklist and the setup journey of section 3 on an iPhone and an Android phone. | Manual, before each release |

---

## 18. Later

Ideas deliberately left out of 1.0, recorded so they are not lost:

- A "Prepare this DSPi for a bridge" button in DSPi Console.
- Improv Wi-Fi over USB serial from the installer page.
- ESP32-C5 (5 GHz) and ESP32-S2 builds.
- Static IP addresses.
- Fetching updates directly from GitHub releases.
- Changing bridge settings from DSPi Console, which needs only Console
  support for the existing messages.
