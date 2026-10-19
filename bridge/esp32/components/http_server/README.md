# http_server

Both HTTP servers (bridge spec 8.2 and 8.8): port 11915 for the Link
WebSocket, `/dspi/v1/info` and the web pages, and port 80 for the setup page
on the setup network and a redirect elsewhere. It hands sessions to its
caller through callbacks, so it does not depend on the hub.

| File | What it does | Portable |
|------|--------------|----------|
| `include/http_server.h`, `http_server.c` | Starts the servers, upgrades WebSockets, serves the embedded pages, and sends each session's queued frames in batches without waiting. | no |
| `include/ws_header.h`, `ws_header.c` | Encodes WebSocket frame headers (RFC 6455). | yes |

Public API: `http_server_start()`, `http_server_send()`,
`http_server_ping()`, `http_server_close()`, `http_server_release_slot()`,
`http_server_service()`; `ws_header_encode()`.

Tests: `test/host/test_ws_header.c`; the rest is checked on a board with
`tools/link_smoke.mjs`.
