# dspi-link-js

The TypeScript client for the DSPi Link protocol, and the browser UI a hub
serves. Both live in one npm workspace so the app always builds against the
library in this repository rather than a published copy.

| Path | What it is |
|------|------------|
| `packages/dspi-link/` | `@dspi/link`, the client library. ESM, no runtime dependencies. |
| `apps/web/` | `@dspi/web`, the UI a hub serves from `/`. Vite, plain DOM, no framework. |

Both read their truth from the repository root: the codec tests decode
`../fixtures/`, and the message types spell the wire keys exactly as the
fixtures show them.

## Build and test

```
npm install          # once, at js/
npm test             # library tests (vitest)
npm run build        # builds the library, then the web bundle
npm run typecheck    # both workspaces
```

`npm run build` must run in that order: the app imports `@dspi/link` from its
built `dist/`, so the library is compiled first.

## The library

```ts
import { LinkClient, decodeStatusFrame, readMasterVolumeDb } from "@dspi/link";

const client = new LinkClient({ reconnect: true });
client.on("tokenRejected", ({ hubId }) => forgetToken(hubId));
await client.connect("ws://studio.local:11915/dspi/v1", {
  clientName: "My app",
  tokenFor: (hubId) => loadToken(hubId), // looked up once the hub's hello names it
});
if (client.state === "awaiting_pairing") {
  const token = await client.pair("482913", "My app", "control");
  if (token) saveToken(client.hubId!, token);
}

const snap = await client.snapshot(0);
const volumeDb = readMasterVolumeDb(snap.bulk!);
```

Each module is usable on its own, and `index.ts` re-exports their public parts:

- `constants.ts` - the sub-protocol, default port (11915), WebSocket and info
  paths, protocol version and library version, each defined once.
- `frames.ts` - the binary data plane (Link spec 8). `decodeFrame` /
  `encodeFrame` round-trip every frame type byte for byte. An unknown frame
  type decodes to `null` so a receiver can drop it; a truncated frame of a
  *known* type throws `LinkFrameError`, which is the distinction a caller
  needs.
- `messages.ts` - the JSON control plane (Link spec 7). Types for every
  message, with snake_case wire keys. `decode` preserves unknown fields and
  turns an unknown `t` into a bare envelope instead of throwing (Link spec 10,
  rule 3); `isMessage(message, "device.added")` narrows a decoded message by
  its type.
- `errors.ts` - `LinkProtocolError` (an `err` reply, with the hub's `code`,
  `detail` and, for a rejected `hub.config.set`, the `field` it names) and
  `LinkCommandError` (a CMD response that was not OK).
- `client.ts` - `LinkClient`: handshake, tag and id correlation, the device
  inventory kept current from events, poll subscriptions, locks, keepalive,
  close-code mapping and backoff reconnection, plus the hub administration
  calls of Link spec 7.9 to 7.14 (`renameHub`, `hubStats`, `getHubConfig`,
  `setHubConfig`, `scanWifi`, `joinWifi`, `setAuth`, `listClients`,
  `revokeClient`, `restartHub`, `resetHub`, `updateHub`) and a `hubConfig`
  event for `hub.config.changed`. Every wait has a timeout: requests,
  commands, the handshake (`handshakeTimeoutMs`) and keepalives, and a socket
  that stops answering keepalives is closed so reconnection starts. Poll
  subscriptions are sent again after a reconnect, each device on its own, and
  when a device comes back online; those of a device the hub no longer has are
  forgotten. A gap in the NOTIFY sequence or in a device's firmware `seq`
  emits a `resync` event, as the hub's own RESYNC does (Link spec 8.3). A
  token is chosen by the hub id in the hello (`tokenFor`), never offered to
  another hub, and dropped only when the hub refuses or revokes it, which the
  `tokenRejected` event reports; never because the connection fell during
  auth. On an open-access hub whose courtesy `auth.pair` fails, the session
  goes on as admin without a session id.
- `hubUpdate.ts` - the hub firmware upload behind `updateHub` (Link spec
  7.14): FWDATA chunks, two ahead of their acks, then the hub's verdict.
- `discovery.ts` - finding a hub without DNS-SD (Link spec 3.2 and 3.3):
  `parseHostInput`, `infoUrl`, `webSocketUrl` and `fetchHubInfo`, which gives
  up after 5 s unless given its own `AbortSignal`.
- `decoders.ts` - the firmware payloads Link carries verbatim: the bulk header,
  master volume and global preamp accessors, the combined status frame, the
  platform reply and v2 notification packets. Every offset is documented with
  the wire version it belongs to.
- `base64.ts` - standard base64 for the `_b64` fields, and base64url tokens.
- `sha256.ts` - SHA-256 for firmware uploads. Browsers offer it only on
  secure origins, and a hub's page is usually plain `http://`.

`LinkClient` uses the global `WebSocket` when there is one and otherwise takes
an injected constructor (`new LinkClient({ webSocket: WS })`), which is how the
tests drive it against an in-process fake hub (`test/fakeHub.ts`).

## The web app

Four screens: connect, device list, one device, and a bridge's settings. The
pages follow bridge spec 10.4: mobile first, light and dark from the system
(colours defined once, in `src/tokens.css`), plain language, a real label on
every control, and every destructive action red and confirmed.

| Path | What it is |
|------|------------|
| `src/main.ts` | The control page: the session, and which screen shows. |
| `src/storage.ts` | The hub address, this browser's name and one token per hub, in `localStorage`. |
| `src/device/` | The device screen: `page.ts`, with `volume.ts` and `meters.ts`. |
| `src/settings/` | The bridge settings screen: `page.ts`, one file per section (`status.ts`, `dspi.ts`, `wifi.ts`, `security.ts`, `bridge.ts`, `maintenance.ts`), `fields.ts` for the shared controls and `state.ts` for the page's state and the checks on what the bridge sends. |
| `src/setup/` | The setup page a bridge serves on its setup network: `main.ts` and `api.ts`. |
| `src/dom.ts`, `dialog.ts`, `status.ts`, `problems.ts` | Building elements, the confirmation dialog, status lines, and error messages in words. |
| `src/pin.ts`, `wifi.ts`, `firmware.ts` | PIN and Wi-Fi rules shared by both pages, and reading an ESP32 image's description. |

- **Connect.** Defaults to the origin that served the page, so a hub serving
  this bundle needs no configuration. `GET /dspi/v1/info` fills in the hub's
  name and kind; another `host:port` can be typed, though a hub such as the
  bridge accepts Link sessions only from pages it served itself, so in
  practice this is the page's own hub. When the hub wants a
  PIN, a pairing panel appears, worded from the hello's `pin_source`: a hub
  with a fixed PIN (a bridge) asks for the PIN the user chose, any other hub
  for the one its pairing window shows. The token it returns is kept in
  `localStorage` keyed on the hub id, never on the address, and forgotten
  when the hub refuses it or revokes it.
- **Device list.** From `device.list`, kept current from `device.added`,
  `device.changed` and `device.removed`.
- **Device.** Master volume as a slider (-127 to 0 dB, mute at -128), read from
  the hub's snapshot, the device's bulk blob (`0xA0`, when the hub gives no
  snapshot and no input count) or a `0xD3` GET, and written with `0xD2`,
  throttled to about 20 writes a second. A meter strip from a poll subscription on `0x50` /
  wValue 9 at 10 Hz, decoded with `decodeStatusFrame`; a clipped channel says
  "Clip" under its bar. A `PARAM_CHANGED` notification for the master volume
  offset moves the slider when another client makes the change; frames whose
  `origin` is our own session id are dropped. `RESYNC` and `BULK_INVALIDATED`
  re-read the state, and the subscription follows a change in channel count.
- **Bridge settings.** Shown to an `admin` session when the hub has
  `hub_config`: status, the DSPi connection with a wiring diagram, Wi-Fi, the
  PIN and paired clients, name, hostname and LED, and firmware update,
  restart, forget Wi-Fi and factory reset. `firmware.ts` reads an ESP32
  image's chip, version and project, so a wrong file is refused before it is
  sent. Everything goes through the admin messages; the layout is in
  `esp32_bridge_spec.md` section 10.3.

`setup.html` is the second page, which only a bridge serves, on its setup
network (bridge spec 10.2). It talks to the bridge's small HTTP setup API
(bridge spec 9.3) rather than Link.

No web fonts, no CDN, no external requests of any kind: the page has to work on
a LAN with no internet.

## How a hub serves the app

`npm run build` writes `apps/web/dist`:

```
dist/index.html
dist/setup.html
dist/assets/index-<hash>.js
dist/assets/index-<hash>.css
dist/assets/setup-<hash>.js
dist/assets/setup-<hash>.css
dist/assets/wifi-<hash>.js      code both pages share (Rollup names it after one module)
```

Asset paths are relative (`base: "./"` in `vite.config.ts`), so the same
directory works served from `/` on any hub with no rewriting. A hub serves it
alongside the protocol on one port (Link spec 4.2):

| Path | Serve |
|------|-------|
| `/dspi/v1` | the WebSocket upgrade |
| `/dspi/v1/info` | the discovery JSON, with `Access-Control-Allow-Origin: *` |
| `/` and `/assets/...` | this bundle; any path without a file extension serves `index.html` |

A hub that serves it advertises `web` in `hello.caps` and `web=1` in its
DNS-SD TXT record.

The control page is about 27 kB gzipped and the setup page about 8 kB,
small enough for a microcontroller hub to hold in flash and serve from a
static table. The library is marked `"sideEffects": false`, so a page that
imports only a constant from it does not carry the client.
