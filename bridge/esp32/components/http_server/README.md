# http_server

Both HTTP servers (bridge spec 8.2 and 8.8): port 11915 for the Link
WebSocket, `/dspi/v1/info` and the web pages, and port 80 for the setup page
on the setup network and a redirect elsewhere. It hands sessions to its
caller through callbacks, so it does not depend on the hub.

| File | What it does | Portable |
|------|--------------|----------|
| `include/http_server.h`, `http_server.c` | Starts the servers, upgrades WebSockets, serves the embedded pages, sends each session's queued frames in batches without waiting, holds each session to a share of the hub's queue and of memory, and closes sockets only from the server's own task, retrying when ESP-IDF's work queue is full. | no |
| `include/ws_header.h`, `ws_header.c` | Encodes WebSocket frame headers (RFC 6455). | yes |
| `include/origin.h`, `origin.c` | Whether a browser's Origin header names the bridge, so only the bridge's own pages may open a Link session (Link spec 4). | yes |

Public API: `http_server_start()`, `http_server_send()`,
`http_server_ping()`, `http_server_close()`, `http_server_release_slot()`,
`http_server_message_done()`,
`http_server_service()`, `http_server_peer_address()`;
`ws_header_encode()`, `origin_is_own()`.

Tests: `test/host/test_ws_header.c` and `test/host/test_origin.c`; the
rest is checked on a board with `tools/link_smoke.mjs` and
`tools/link_conformance.mjs`.
