# setup_api

The setup network's small HTTP API (bridge spec 9.3), used by the setup
page: the bridge's state, nearby networks, applying the choices and
finishing. It answers only on the setup network, only to requests
addressed to the setup address (192.168.4.1), and takes POSTs only as
`application/json`, so a web page elsewhere cannot call it.

| File | What it does | Portable |
|------|--------------|----------|
| `include/setup_api.h`, `setup_api.c` | The `/setup/v1/` endpoints, the PIN checks they need, and the redirect phones' connectivity checks get. | no |

Public API: `setup_api_handle()`, which `http_server` calls for every
request on port 80 of the setup network.

Tests: none on the host, as every endpoint needs the HTTP server and
Wi-Fi. Checked on a board: the page opens by itself on an iPhone and an
Android phone, lists networks, joins one, refuses a wrong password, and asks
for the PIN when it is on (bridge spec 3 and 9.3).
