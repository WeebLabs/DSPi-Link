/* Manual host entry and the URLs derived from it, Link spec 3.2 and 3.3. */

import { describe, expect, it } from "vitest";
import { infoUrl, parseHostInput, webSocketUrl } from "../src/discovery.js";

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
