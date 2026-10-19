# web_assets

The web pages the bridge serves (bridge spec 10.5): `js/apps/web/dist`,
gzipped into the firmware at build time.

| File | What it does | Portable |
|------|--------------|----------|
| `include/web_assets.h`, `web_assets.c` | Finds an embedded file by path, with its content type and ETag. | no |
| `web_assets_table.h`; `web_assets_data.c` (in the build folder) | The table's declaration, private to this component; the files themselves, written by `tools/embed_web.py` from `dist/` at every build. | no |

The build stops with the command to run if the web pages have not been
built (`cd js && npm ci && npm run build`).

Public API: `web_assets_find()`.

Tests: none on the host. `tools/embed_web.py` runs at every firmware build,
and `tools/link_smoke.mjs` and a browser check the pages on a board.
