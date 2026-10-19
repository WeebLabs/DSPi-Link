# setup_api

The setup network's small HTTP API (bridge spec 9.3), used by the setup
page: the bridge's state, nearby networks, applying the choices and
finishing. It answers only on the setup network.

| File | What it does | Portable |
|------|--------------|----------|
| `include/setup_api.h`, `setup_api.c` | The `/setup/v1/` endpoints, the PIN checks they need, and the redirect phones' connectivity checks get. | no |

Public API: `setup_api_handle()`, which `http_server` calls for every
request on port 80 of the setup network.

Tests: checked on a board and against a stand-in for the API (see
`PLAN.md`, phases 4 and 5).
