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

**Status (2026-10-19):** done, with the items below left for the bench.
On the ESP32-S3:

- From the settings page: the bridge was renamed (the name reached `hello`,
  the info endpoint and DNS-SD at once), the PIN was turned on, used to pair
  a second browser, one paired browser was removed, and the PIN was turned
  off again. A stored token reconnected without a prompt; a wrong PIN was
  refused. Restart from the page came back to the settings page by itself.
- Setting 921,600 baud with detection on: the bridge lost the DSPi, found it
  again at 1,000,000 and the page showed the found rate. This exposed a gap,
  now fixed: a detected rate was saved but the hub core and the page were
  not told.
- PIN checks took 1.5 s with 10,000 PBKDF2 rounds and held up every session
  meanwhile; the bridge now uses 1,000 (bridge spec 8.9 says why that is
  enough).
- The setup page's PIN step was checked against a stand-in for the setup
  API at phone width, for a new bridge and for one that already has a PIN.
- Left over: the setup PIN step on a real phone; changing network, the UART
  pins and the PIN itself from the settings page; Forget Wi-Fi and Factory
  reset from the page, which need a fresh phone setup afterwards. The LED
  setting is saved but nothing drives the LED until phase 6.
- Outside this repository: DSPi Console should default to `auth: none` and
  word its PIN prompt from `pin_source`.

## Phase 6: Updates, reset and LED

- `ota` and `hub.update` (section 14), with rollback.
- `button` and the reset levels (12.1), and `status_led` (13).
- "Update firmware" in the settings page's Maintenance section, beside the
  restart and reset actions phase 5 added.

**Done when** an update from the settings page succeeds, a deliberately
broken image rolls back by itself, and both BOOT button levels work with
the LED showing each stage.

**Status (2026-10-19):** done, with the items below left for the bench.
On the ESP32-S3:

- Updates over Link with the library's `updateHub()`, the call the
  settings page makes: a 983 KB image took 4 to 7 s to send and check, and
  the bridge was back on the new version about 7 s later. Going down a
  version works the same way.
- A new image confirmed itself after 60 s and survived a restart. A restart
  41 s after an update went back to the previous image, as it should, and
  an image built to abort 20 s after boot rolled back by itself.
- Refused, with the bridge still on its firmware: an ESP32-C3 image ("not
  firmware for this chip"), random bytes, and a second update while a new
  image was still on trial ("try again in a minute"). The page also refuses
  a file that is not bridge firmware before sending it.
- The BOOT button, held 35 s: the serial log showed each stage at 5, 15 and
  30 s and the release did nothing. The bridge now logs every stage, for
  boards without an LED.
- Left over: the page's own file picker (the browser pane cannot drive it;
  the code path behind it was run with a file handed to it), releasing BOOT
  at the forget-Wi-Fi and factory-reset stages, and the LED itself, since
  this board has none. The LED patterns have host tests.

## Phase 7: Release tooling and documentation

- Release CI: per-chip merged and update images attached to a GitHub release.
- The ESP Web Tools installer page on GitHub Pages.
- `bridge/esp32/README.md` (build, flash, wiring, troubleshooting) and a
  `README.md` in each component.
- The repository `README.md` lists the bridge as an implementation.

**Done when** someone who has never seen the project can go from an unopened
ESP32 board to controlling their DSPi from Console using only the
installer page and the README.

**Status (2026-10-19):** done except the first real release, which is the
user's to make.

- `tools/package_release.sh` makes, for each chip, a full image to flash at
  0x0 and an update image, with `SHA256SUMS`, and the installer site with
  its ESP Web Tools manifest. The full images of a local package booted in
  QEMU on the ESP32, ESP32-S3 and ESP32-C3; `tools/qemu_smoke.sh` now takes
  a full image as a second argument.
- `.github/workflows/esp32-bridge-release.yml` runs on a `bridge-v<version>`
  tag: it checks the tag against `PROJECT_VER`, builds, packages, boots the
  full images in QEMU, creates the GitHub release and publishes the
  installer to GitHub Pages. It has not run yet: it needs a tag, and Pages
  set to deploy from GitHub Actions.
- The installer page (`installer/index.html`) was checked from a local
  server: ESP Web Tools 10.4.0 loads and shows its Install button, and the
  wiring table follows the chosen chip. Installing from it has not been
  tried on a board.
- The bridge `README.md` now covers installing, wiring, the DSPi's UART,
  first use, the BOOT button and LED, updates, troubleshooting, building,
  flashing by hand, testing and releasing. Every component has a
  `README.md`. The repository `README.md` lists the bridge.
- Left over: the "done when" walk-through itself, from an unopened board
  with only the installer and the README, belongs with the release testing
  of phase 8.

## Phase 8: Release testing

- The bench tests of section 17 on all four chips, with the DSPi and with
  the simulator.
- The Link spec 12 conformance checklist, item by item.
- A final review of every file against section 16.

**Done when** every item passes on every chip and the specs are marked as
implemented.

**Status (2026-10-19):** under way. The ESP32-S3 has passed everything below;
the ESP32, ESP32-C3 and ESP32-C6 are waiting for boards.

- `tools/link_conformance.mjs` runs the hub half of the Link spec 12
  checklist against a live bridge. On the ESP32-S3 every item passes:
  - discovery and the info endpoint;
  - the sub-protocol and hello first;
  - `max_inflight` and ordering;
  - poll fan-out and stop;
  - the device going offline, being removed and returning on the same
    handle;
  - close codes and a reconnect storm, with no lost session and free heap
    unchanged.

  It found two gaps, now fixed: a WebSocket without `dspi-link-1` was
  accepted, and a burst of disconnects could overflow ESP-IDF's small
  control queue and lose a close until the keepalive reclaimed it.
- The client half of the checklist is met by the web client: manual entry,
  tokens keyed on `hid`, the prompt worded from `pin_source`, `caps` and
  `limits` used, echoes suppressed by `origin`, state re-read on gaps and
  RESYNC, and device features gated on the device's own version. Browsers
  cannot browse DNS-SD, so the web client relies on manual entry and the
  page's own origin; DSPi Console covers DNS-SD.
- Every file was reviewed against section 16. The review found about 70
  bugs and many smaller standards issues, and all were fixed. Among the
  bugs:
  - the setup page's PIN check failing open when the hub was busy, and a
    failed hash letting any PIN through;
  - heap use and a possible NULL dereference on the notification path;
  - lifecycle events that could be dropped;
  - a Wi-Fi retry that could stop for good;
  - unsafe reads across tasks;
  - a stale socket confusing the JS client;
  - a meter gradient that showed red at any level.

  145 host tests and 150 JS tests pass. All four chips build without
  warnings, QEMU passes on three, and `link_smoke.mjs` and
  `link_conformance.mjs --offline` pass on the ESP32-S3.
- A security and robustness audit of every subsystem followed (six
  parallel reviews). Its critical and high findings are fixed and were
  checked on the ESP32-S3, each against the attack that had worked:
  - one deeply nested JSON message crashed the bridge before pairing; JSON
    may now nest 8 levels;
  - any web page in a browser on the network could administer the bridge;
    WebSocket upgrades from a foreign `Origin` are now refused;
  - idle connections could hold every socket on the Link port, and one
    client trickling a request could freeze every session; the Link port
    now drops the least recently used socket, uses TCP keep-alive, and
    gives each request 8 s;
  - an abandoned update could still be switched to, and a later wrong file
    left as the boot image; abandoned updates now write and switch nothing,
    and a new update waits while a finished one awaits its restart;
  - a stranger guessing PINs could lock paired clients out; the hub-wide
    lock now guards PIN guesses only.

  `tools/build.sh` now regenerates a chip's sdkconfig when the defaults
  change: the earlier bench runs had used a stale configuration (16
  sockets). The bench was rerun on a clean build and passes.
- The audit's medium and low findings are fixed too, again by six parallel
  agents, one per subsystem. Among them:
  - UART: a frame that fails its CRC or stalls is searched again, so noise
    cannot swallow the next frame; answers that arrived before a deadline
    still count; the UART interrupt runs from IRAM; the heartbeat reads the
    serial, so a swapped DSPi is noticed; a damaged answer is reported as
    TIMEOUT, never CRC_ERROR.
  - Hub: one poll budget for the whole link; fresh status for every
    snapshot; write-as-read commands count as writes; client names are
    valid UTF-8; unreadable PIN data fails closed; a client forgotten to
    make room is disconnected.
  - Glue: the uart task never waits on the hub; each session has a share
    of the hub's queue and a byte cap on its send queue; refused frames are
    drained so the close code reaches the client.
  - Network and setup: the Wi-Fi name and password are saved as one blob;
    a downgrade keeps the hub id without erasing everything; the setup API
    checks Host, Content-Type and the subnet.
  - Updates and boot: a new image is judged on being reachable and every
    task running, from 60 s up to 5 min; a supervisor restarts a stuck
    bridge; BOOT works from the first boot step; a failed boot step waits
    60 s before restarting; the installer serves a pinned, checked ESP Web
    Tools; workflow actions are pinned by commit.
  - Web app and library: NOTIFY seq gaps trigger a resync; polls are
    restored per device; a rejected token is reported.

  `tools/build.sh` now also drops the CMake cache when it regenerates a
  sdkconfig, since idf.py stops if the sdkconfig its cache names is
  missing.

  186 host tests pass, also under ASan and UBSan, and so do 167 JS tests.
  All four chips build without warnings (the ESP32 keeps 31 KB of IRAM
  free with the UART interrupt in IRAM), and QEMU passes on three. On the
  ESP32-S3, installed over the air:
  - `link_smoke.mjs`, `link_conformance.mjs --offline` and the burst test
    pass;
  - the four audit attacks are still refused;
  - a fragmented or oversized frame now gets close 4000, not a reset;
  - a client sending 2000 messages at once is held back and served in
    full;
  - the new image confirmed itself, and an update during a trial is
    refused.
- Release note: the ESP32 no longer offers GPIO 34 to 39 for the UART. A
  bridge with one of them saved as its RX pin returns both UART pins to the
  defaults after the update.
- Open from the audit: whether to sign firmware updates (it needs a
  release key held in CI).
- Left: the same bench run on the ESP32, ESP32-C3 and ESP32-C6, the
  setup journey on an Android phone, DSPi Console against the PIN, and
  marking the specs implemented once every chip passes.

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
