# DSPi Link

The local-network protocol through which a hub (DSPi Console in gateway
mode, or a standalone bridge) shares DSPi devices with clients (another
Console, a browser, a phone). This repository is the shared home for
everything an implementation in any language needs:

| Path | What it is |
|------|------------|
| `spec/dspi_link_protocol_spec.md` | The protocol, the single normative text. |
| `spec/esp32_bridge_spec.md` | The ESP32 bridge: a reference bridge hub that serves one DSPi over UART and Wi-Fi. |
| `policy/commands.json` | Every vendor opcode with its direction(s) and authorization class (`read`, `control`, `config`), per direction where the two differ. Hubs embed it; clients use it to grey out controls. A code absent from it is `config`. |
| `fixtures/frames/` | Byte-exact binary frames of every type, with `frames.json` giving each one's decoded meaning. |
| `fixtures/messages/` | Control-plane JSON samples, one file per message type. |
| `fixtures/device/` | Real captures from a board (bulk blob, status frame, platform), with a sidecar giving the decoded values, so every client's decoders agree on the same bytes. |
| `js/` | `dspi-link-js`: the TypeScript client library and the web UI the hub serves. |
| `bridge/esp32/` | The ESP32 bridge firmware (ESP-IDF): a hub for one DSPi, wired to its UART and joined to Wi-Fi. Its `README.md` covers installing, wiring and troubleshooting; `PLAN.md` gives the state of each part. |
| `conformance/` | Planned, not yet in the repository: reference client and hub for testing implementations against (Python). |

Implementations: DSPi Console for macOS (`WeebLabs/DSPi-Console`, hub and
client), DSPi Console for Windows (planned), the web UI here, and the ESP32
bridge here (`bridge/esp32/`), a hub that runs on ESP32, ESP32-S3,
ESP32-C3 and ESP32-C6 boards. Each other implementation vendors a tagged
version of this repository.

To turn an ESP32 board into a bridge for your DSPi, use the installer page,
[weeblabs.github.io/DSPi-Link/installer](https://weeblabs.github.io/DSPi-Link/installer/),
or see [`bridge/esp32/README.md`](bridge/esp32/README.md).

License: MIT, see `LICENSE`.
