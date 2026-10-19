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
await client.connect("ws://studio.local:11915/dspi/v1", {
  clientName: "My app",
  token: storedToken,          // omit to pair with a PIN instead
});
if (client.state === "awaiting_pairing") {
  const token = await client.pair("482913", "My app", "control");
}

const snap = await client.snapshot(0);
const volumeDb = readMasterVolumeDb(snap.bulk!);
```

Five modules, each usable on its own:

- `frames.ts` - the binary data plane (spec 8). `decodeFrame` / `encodeFrame`
  round-trip every frame type byte for byte. An unknown frame type decodes to
  `null` so a receiver can drop it; a truncated frame of a *known* type throws
  `LinkFrameError`, which is the distinction a caller needs.
- `messages.ts` - the JSON control plane (spec 7). Types for every message,
  with snake_case wire keys. `decode` preserves unknown fields and turns an
  unknown `t` into a bare envelope instead of throwing, per spec 10.3.
- `client.ts` - `LinkClient`: handshake, tag and id correlation, the device
  inventory kept current from events, poll subscriptions, locks, keepalive,
  close-code mapping and backoff reconnection. Since 1.2 it also has the hub
  administration calls of spec 7.9 to 7.13 (`getHubConfig`, `setHubConfig`,
  `scanWifi`, `joinWifi`, `setAuth`, `listClients`, `revokeClient`,
  `restartHub`, `resetHub`, `updateHub`) and a `hubConfig` event for
  `hub.config.changed`. A rejected `hub.config.set` rejects with a
  `LinkProtocolError` whose `field` names the key. `updateHub` sends an
  image as FWDATA chunks, two ahead of their acks, and resolves with the
  new version.
- `sha256.ts` - SHA-256 for firmware uploads. Browsers offer it only on
  secure origins, and a hub's page is usually plain `http://`.
- `decoders.ts` - the firmware payloads Link carries verbatim: the bulk header,
  master volume and global preamp accessors, the combined status frame, the
  platform reply and v2 notification packets. Every offset is documented with
  the wire version it belongs to.

`LinkClient` uses the global `WebSocket` when there is one and otherwise takes
an injected constructor (`new LinkClient({ webSocket: WS })`), which is how the
tests drive it against an in-process fake hub.

## The web app

Four screens: connect, device list, one device, and a bridge's settings.

- **Connect.** Defaults to the origin that served the page, so a hub serving
  this bundle needs no configuration. `GET /dspi/v1/info` fills in the hub's
  name and kind; another `host:port` can be typed instead. When the hub wants a
  PIN, a pairing panel appears, worded from the hello's `pin_source`: a hub
  with a fixed PIN (a bridge) asks for the PIN the user chose, any other hub
  for the one its pairing window shows. The token it returns is kept in
  `localStorage` keyed on the hub id, never on the address.
- **Device list.** From `device.list`, kept current from `device.added`,
  `device.changed` and `device.removed`.
- **Device.** Master volume as a slider (-127 to 0 dB, mute at -128), read from
  the hub's snapshot or a `0xD3` GET and written with `0xD2`, throttled to about
  20 writes a second. A meter strip from a poll subscription on `0x50` /
  wValue 9 at 10 Hz, decoded with `decodeStatusFrame`. A `PARAM_CHANGED`
  notification for the master volume offset moves the slider when another
  client makes the change; frames whose `origin` is our own session id are
  dropped. `RESYNC` and `BULK_INVALIDATED` re-read the snapshot.
- **Bridge settings** (`settings.ts`). Shown to an `admin` session when the
  hub has `hub_config`: status, the DSPi connection with a wiring diagram,
  Wi-Fi, the PIN and paired clients, name, hostname and LED, and firmware
  update, restart, forget Wi-Fi and factory reset. `firmware.ts` reads an
  ESP32 image's chip, version and project, so a wrong file is refused
  before it is sent. Everything goes through the admin
  messages; the layout is in `esp32_bridge_spec.md` section 10.3.

`apps/web` also builds a second page, `setup.html`, which only a bridge
serves, on its setup network (bridge spec 10.2). It talks to the bridge's
small HTTP setup API rather than Link.

No web fonts, no CDN, no external requests of any kind: the page has to work on
a LAN with no internet.

## How a hub serves the app

`npm run build` writes `apps/web/dist`:

```
dist/index.html
dist/assets/index-<hash>.js
dist/assets/style-<hash>.css
```

Asset paths are relative (`base: "./"` in `vite.config.ts`), so the same
directory works served from `/` on any hub with no rewriting. A hub serves it
alongside the protocol on one port (spec 4.2):

| Path | Serve |
|------|-------|
| `/dspi/v1` | the WebSocket upgrade |
| `/dspi/v1/info` | the discovery JSON, with `Access-Control-Allow-Origin: *` |
| `/` and `/assets/...` | this bundle; any path without a file extension serves `index.html` |

A hub that serves it advertises `web` in `hello.caps` and `web=1` in its
DNS-SD TXT record.

The control page is about 19 kB gzipped and the setup page about 7 kB,
small enough for a microcontroller hub to hold in flash and serve from a
static table.
