/* Manual host entry and the URLs derived from it, Link spec 3.2 and 3.3. */

import { afterEach, describe, expect, it, vi } from "vitest";
import { fetchHubInfo, infoUrl, parseHostInput, webSocketUrl } from "../src/discovery.js";

describe("manual host entry, spec 3.3", () => {
  it("defaults a bare host to the DSPi port", () => {
    expect(parseHostInput("studio.local")).toEqual({
      host: "studio.local",
      port: 11915,
      secure: false,
    });
  });

  it("keeps an explicit port", () => {
    expect(parseHostInput("192.168.1.9:8080").port).toBe(8080);
  });

  it("accepts a URL and takes TLS from its scheme", () => {
    expect(parseHostInput("https://studio.local:8443/anything")).toEqual({
      host: "studio.local",
      port: 8443,
      secure: true,
    });
  });

  it("handles a bracketed IPv6 literal", () => {
    expect(parseHostInput("[fe80::1]:11915")).toEqual({
      host: "[fe80::1]",
      port: 11915,
      secure: false,
    });
  });

  it("brackets a bare IPv6 address instead of reading a port from it", () => {
    expect(parseHostInput("fe80::1")).toEqual({ host: "[fe80::1]", port: 11915, secure: false });
  });

  it("builds the info and WebSocket URLs", () => {
    expect(infoUrl("studio.local")).toBe("http://studio.local:11915/dspi/v1/info");
    expect(webSocketUrl("studio.local")).toBe("ws://studio.local:11915/dspi/v1");
  });

  it("honours tls and a non-default path from the info reply", () => {
    const info = {
      proto: { major: 1, minor: 0 },
      hub: { id: "x", name: "Hub", kind: "bridge" as const },
      auth: "pin" as const,
      tls: true,
      ws: "/dspi/v1alt",
    };
    expect(webSocketUrl("studio.local:8443", info)).toBe("wss://studio.local:8443/dspi/v1alt");
  });
});

describe("fetchHubInfo", () => {
  afterEach(() => {
    vi.useRealTimers();
    vi.unstubAllGlobals();
  });

  /** A fetch that never answers until its signal aborts. */
  function hangingFetch(): void {
    vi.stubGlobal(
      "fetch",
      (_url: string, init: { signal: AbortSignal }) =>
        new Promise((_resolve, reject) => {
          init.signal.addEventListener("abort", () => reject(init.signal.reason));
        }),
    );
  }

  it("gives up after 5 s without AbortSignal.timeout, which Safari 15 lacks", async () => {
    vi.useFakeTimers();
    vi.stubGlobal("AbortSignal", { ...AbortSignal, timeout: undefined });
    hangingFetch();
    const result = fetchHubInfo("studio.local").catch((error: Error) => error);
    await vi.advanceTimersByTimeAsync(5000);
    expect(await result).toMatchObject({ message: "hub info timed out" });
  });

  it("uses the caller's signal instead when given one", async () => {
    hangingFetch();
    const controller = new AbortController();
    const result = fetchHubInfo("studio.local", false, controller.signal).catch((e: Error) => e);
    controller.abort(new Error("stopped"));
    expect(await result).toMatchObject({ message: "stopped" });
  });
});
