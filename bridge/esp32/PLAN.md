# ESP32 Bridge Implementation Plan

This plan builds the bridge described in `spec/esp32_bridge_spec.md` in eight
phases. Each phase ends with something that runs and can be checked, so a
mistake shows up in the phase that made it. Section numbers below refer to
the bridge spec unless they say "Link spec".

The code standards in section 16 of the bridge spec apply from the first
commit. Each phase is reviewed against them, not only against its tests.

---

## Phase 0: Agree the specs

- Review the Link 1.2 changes in `spec/dspi_link_protocol_spec.md` and the new
  `spec/esp32_bridge_spec.md`.
- Settle the open points, now recorded as decisions at the end of this plan.

**Done when** both documents are approved and committed.

## Phase 1: Project skeleton

- Create `bridge/esp32/` as an ESP-IDF project with the layout in section
  6.3, `idf_version.txt`, `partitions.csv` and the per-chip
  `sdkconfig.defaults` files. Each later component is created by the phase
  that implements it, so the tree never holds empty placeholders.
- Add `bridge_config`, `board` (the pin table of section 4.4, checked against
  each datasheet) and `settings` (section 11).
- Add `.clang-format`, the warning flags and the host test runner in
  `test/host` with one passing test.
- Add CI: build all four chips, run host tests, check the image size.

**Done when** CI builds all four chips, and a flashed board boots, logs its
chip, PSRAM and default pins, and survives a restart with settings intact.

**Status (2026-10-18):** done except the check on real boards. All four
chips build with ESP-IDF v6.1 (155 to 180 KB images), the host tests pass,
and `tools/qemu_smoke.sh` boots the ESP32, ESP32-S3 and ESP32-C3 images
twice in QEMU with the hub id kept. Flashing a real board of each chip is
still to do.

## Phase 2: The DSPi UART link

- `dspi_frame`: encoder, byte-at-a-time parser and CRC, with host tests from
  the firmware spec's worked examples and corrupted, split and noisy
  streams.
- `dspi_uart`: the portable link engine (`dspi_link.c`) with the timing of
  section 7.2, retries (7.3), quarantine (7.4), baud detection (7.5) and
  identity reading (7.6), and the uart task around it.
- `test/host/fake_dspi.c`: a simulated DSPi on a virtual clock that can stay
  silent, answer late, corrupt its CRC or return any status. It takes the
  place of the planned USB-UART simulator, since no adapter is available.
- `main/phase2_bench.c`, temporary: starts the uart task and logs the device
  being found, a bulk read and each notification. Removed in phase 3.

The front panel firmware's timeout recovery (`DSPi-ESP32-Front-Panel`,
`dspiRecoverAfterTimeout`) is a useful reference, but the code is written
fresh to these standards, with a receive buffer sized for 132-byte replies
and 6136-byte bulk reads.

**Done when** the bridge finds a real DSPi, reads its serial, platform and
bulk blob, and logs notifications while the DSPi is changed from Console,
and the host tests cover every fault the simulated DSPi can inject.

**Status (2026-10-18):** done. On an ESP32-S3 (octal PSRAM, 16 MB flash)
wired to a DSPi running 1.1.6-beta5:

- The bridge found the DSPi at 115200 baud by detection and saved the rate.
  A bulk read took 535 ms.
- 98 volume notifications from Console arrived with consecutive `seq`
  values and no gaps.
- When the DSPi was switched to 1,000,000 baud, the bridge reported it lost
  after three timeouts, found it again at the new rate 210 ms later and
  saved that rate. A bulk read then took 62 ms (61 ms predicted).

## Phase 3: The Link hub

- `tools/gen_policy.py` and `link_policy`, with a host test that the
  generated table matches `policy/commands.json`.
- `link_codec`, tested against every file in `fixtures/`.
- `hub` with `auth: none` only: hello, inventory, commands (section 8.3),
  notifications with origin (8.4), polls (8.5), snapshot (8.6), locks,
  device names and stats (8.7). Host tests with a simulated DSPi and fake
  clock.
- `http_server` on port 11915: the WebSocket with sub-protocol
  `dspi-link-1`, keepalive, `/dspi/v1/info` and static files.
- `net` in station mode only, with credentials compiled in from a
  git-ignored `local_wifi.h` for development. mDNS and DNS-SD.

**Done when** DSPi Console finds the bridge and controls the DSPi through it,
the `dspi-link-js` test suite passes against it, and the existing web client
served from the bridge shows volume and meters.

**Status (2026-10-18):** done, with one substitution: the `dspi-link-js`
unit suite runs against its own fake hub, so `tools/link_smoke.mjs` drives
the live bridge through the library instead. On the ESP32-S3 and DSPi from
phase 2:

- DSPi Console found the bridge by DNS-SD and controlled the DSPi through
  it, and the web client served from the bridge showed volume and meters.
- `tools/link_smoke.mjs` passed all 12 checks on six runs: two sessions,
  the device list, a platform read, a snapshot in about 90 ms, a
  notification reaching both sessions with the writer's session as origin,
  meter polls at the granted 10 Hz, a lock refusing another session's write
  with `LOCKED`, and `hub.stats`.
- One earlier run had a snapshot request time out, and it did not happen
  again. A JSON message the bridge cannot queue is now logged, so a repeat
  will show its cause.
- ESP-IDF 6.1 no longer calls the WebSocket handler at the handshake, so
  sessions open in the post-handshake hook.


## Phase 4: Wi-Fi setup

- `net`: the states of section 9.1, the setup network, captive DNS and the
  connectivity-check redirects (9.2).
- `setup_api` (9.3), answering only on the setup network.
- The setup page (10.2) as a second entry point in `js/apps/web`, and
  `tools/embed_web.py` with the `web_assets` component.
- Port 80: the setup page on the setup network, a redirect on the home
  network.
- `local_wifi.h` is removed.

**Done when** a freshly erased board can be set up from an iPhone and an
Android phone without typing any address, falls back to the setup network
3 minutes after its home network disappears, and returns to the home network
when it comes back.

**Status (2026-10-18):** done on an iPhone, with three items left over.
On the ESP32-S3:

- The phone opened the setup page by itself, scanned, joined the home
  network in about 5 s, and saved it; after a restart the bridge rejoined
  directly. The setup network went off by itself 2 minutes after the join.
- The first try showed a white page: the phone's captive-portal browser
  opens several connections at once and the port 80 server, limited to 4
  and never dropping idle ones, stalled the page's script. Port 80 now
  allows 7 and drops the idlest.
- Load testing then found the send path wrong under pressure: every frame
  was its own small write and its own entry in the HTTP server's six-entry
  work queue. Sends are now batched per session and never wait (bridge spec
  8.2). Two clients polling three meters at 20 Hz each, plus snapshots and
  the end-to-end test, ran without a dropped frame.
- Left over: the Android check, and the fallback after 3 minutes without
  the home network, which the host tests cover but the bench did not. The
  PIN step of the setup page arrives with the PIN in phase 5.


## Phase 5: Settings and security

- `hub_auth` (section 8.9) and the `pin` mode with a fixed PIN, with host
  tests for hashing, rate limits and revocation.
- The administration messages of Link spec 7.10 to 7.13 in `hub` (section
  8.10), with the configuration keys of 8.11.
- `dspi-link-js` 1.2: types and client methods for the new messages and
  `pin_source`, with new files in `fixtures/messages/` for each new message.
- The settings screens (10.3) in the web app.

**Done when** every setting can be changed from the settings page and takes
effect without a restart where section 8.11 says so, turning the PIN on
signs out every other browser and app, and the PIN prompt in the web client
uses the `fixed` wording.

## Phase 6: Updates, reset and LED

- `ota` and `hub.update` (section 14), with rollback.
- `button` and the reset levels (12.1), and `status_led` (13).
- The Maintenance section of the settings page.

**Done when** an update from the settings page succeeds, a deliberately
broken image rolls back by itself, and both BOOT button levels work with
the LED showing each stage.

## Phase 7: Release tooling and documentation

- Release CI: per-chip merged and update images attached to a GitHub release.
- The ESP Web Tools installer page on GitHub Pages.
- `bridge/esp32/README.md` (build, flash, wiring, troubleshooting) and a
  `README.md` in each component.
- The repository `README.md` lists the bridge as an implementation.

**Done when** someone who has never seen the project can go from an unopened
ESP32 board to controlling their DSPi from Console using only the
installer page and the README.

## Phase 8: Release testing

- The bench tests of section 17 on all four chips, with the DSPi and with
  the simulator.
- The Link spec 12 conformance checklist, item by item.
- A final review of every file against section 16.

**Done when** every item passes on every chip and the specs are marked as
implemented.

---

## Work outside this repository

These belong to DSPi Console and are listed so nothing is forgotten:

- Default `auth` to `none` (Link spec 6.1).
- Word the PIN prompt from `pin_source` (Link spec 6.2).

## Decisions (2026-10-18)

1. **ESP-IDF v6.1**, the newest stable release when phase 1 started, pinned
   in `idf_version.txt`. Builds run in Espressif's official Docker image of
   that version, so no local installation is needed.
2. **Default UART pins** as in section 4.4 of the bridge spec.
3. **Test hardware:** one ESP32 board and a DSPi are available. There is no
   USB-UART adapter, so fault injection runs against the simulated DSPi in
   the host tests instead. Testing every chip in phase 8 needs a board of
   each.
