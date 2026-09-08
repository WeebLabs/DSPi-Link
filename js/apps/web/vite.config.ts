import { defineConfig } from "vite";

export default defineConfig({
  // Relative asset paths: the same dist/ is served from / by any hub, including
  // a microcontroller that knows nothing about the URL it is reached on.
  base: "./",
  build: {
    outDir: "dist",
    assetsDir: "assets",
    target: "es2020",
    sourcemap: false,
    // One JS file and one CSS file keeps a bridge's static file table trivial.
    cssCodeSplit: false,
  },
});
