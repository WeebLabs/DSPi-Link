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
    rollupOptions: {
      // Two pages: the control page every hub serves, and the setup page a
      // bridge serves on its setup network (bridge spec 10.1). Paths are
      // relative to this folder, which is Vite's root.
      input: {
        index: "index.html",
        setup: "setup.html",
      },
    },
    // Each page gets only its own stylesheet; they do not share a look.
    cssCodeSplit: true,
  },
});
