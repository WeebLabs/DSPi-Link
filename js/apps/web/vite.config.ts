/*
 * Builds the two pages a hub serves (bridge spec 10.1 and 10.5): the control
 * page every hub serves from /, and the setup page a bridge serves on its
 * setup network. The firmware build embeds the result, dist/, into flash.
 */

import { defineConfig } from "vite";
import packageJson from "./package.json";

export default defineConfig({
  // Relative asset paths: the same dist/ is served from / by any hub, including
  // a microcontroller that knows nothing about the URL it is reached on.
  base: "./",
  // The page's version, sent in hello, has one owner: package.json.
  define: {
    __APP_VERSION__: JSON.stringify(packageJson.version),
  },
  build: {
    outDir: "dist",
    assetsDir: "assets",
    target: "es2020",
    sourcemap: false,
    rollupOptions: {
      // Paths are relative to this folder, which is Vite's root.
      input: {
        index: "index.html",
        setup: "setup.html",
      },
    },
    // Each page gets only its own stylesheet; the setup page must stay small.
    cssCodeSplit: true,
  },
});
