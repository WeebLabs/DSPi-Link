import { describe, expect, it } from "vitest";
import {
  LINK_SUBPROTOCOL,
  LinkClient,
  LinkCommandError,
  LinkProtocolError,
  closeReasonFor,
} from "../src/client.js";
import { LinkStatus } from "../src/frames.js";
import { bytesToBase64 } from "../src/decoders.js";
import type { DeviceInfo } from "../src/messages.js";
import { FakeHub, settle } from "./fakeHub.js";
import { fixtureBytes, fixtureJson } from "./fixtures.js";

const DEVICE: DeviceInfo = {
  handle: 0,
  serial: "0123456789ABCDEF",
  name: "Bench",
  platform: 1,
  fw: "1.1.6",
  fw_beta: 5,
  outputs: 9,
  inputs: 8,
  wire_version: 32,
  state: "online",
  link: "usb",
  locked_by: null,
};

function newClient(hub: FakeHub): LinkClient {
  // Pings and reconnects are off so a test never depends on a timer.
  return new LinkClient({ webSocket: hub.webSocket, pingIntervalMs: 0, reconnect: false });
}

describe("handshake", () => {
  it("requests the dspi-link-1 sub-protocol", async () => {
    const hub = new FakeHub({ auth: "none", devices: [DEVICE] });
    const client = newClient(hub);
    await client.connect("ws://hub.local:11915/dspi/v1", { clientName: "Test" });
    expect(hub.socket.protocols).toBe(LINK_SUBPROTOCOL);
    client.disconnect();
  });

  it("reaches ready with the session id and the device list, using a token", async () => {
    const hub = new FakeHub({ auth: "pin", validToken: "GOOD", session: 12, devices: [DEVICE] });
    const client = newClient(hub);
    const states: string[] = [];
    client.on("state", (s) => states.push(s.state));

    await client.connect("ws://hub.local:11915/dspi/v1", { clientName: "Test", token: "GOOD" });

    expect(client.state).toBe("ready");
    expect(client.session).toBe(12);
    expect(client.role).toBe("control");
    expect(client.hubId).toBe("6f1a2b3c-4d5e-4f60-8a9b-0c1d2e3f4a5b");
    expect(client.hasCap("snapshot")).toBe(true);
    expect(client.limits?.max_payload).toBe(8192);
    expect(client.deviceList).toEqual([DEVICE]);
    expect(states).toEqual(["connecting", "handshaking", "authenticating", "ready"]);

    // hello first, then auth, then the inventory (spec 5).
    expect(hub.received.map((m) => m.t)).toEqual(["hello", "auth.token", "device.list"]);
    client.disconnect();
  });

  it("waits for a PIN when the hub wants one and no token is stored", async () => {
    const hub = new FakeHub({ auth: "pin", pin: "482913", issuedToken: "TOKEN-xyz" });
    const client = newClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Web UI" });

    expect(client.state).toBe("awaiting_pairing");
    expect(client.session).toBeNull();

    const token = await client.pair("482913", "Web UI", "control");
    expect(token).toBe("TOKEN-xyz");
    expect(client.token).toBe("TOKEN-xyz");
    expect(client.state).toBe("ready");
    expect(client.session).toBe(12);
    client.disconnect();
  });

  it("falls back to awaiting_pairing when the stored token is rejected", async () => {
    const hub = new FakeHub({ auth: "pin", validToken: "GOOD", pin: "111111" });
    const client = newClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test", token: "STALE" });

    expect(client.state).toBe("awaiting_pairing");
    expect(client.token).toBeNull();
    client.disconnect();
  });

  it("rejects a wrong PIN and stays awaiting_pairing", async () => {
    const hub = new FakeHub({ auth: "pin", pin: "482913" });
    const client = newClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });
    await expect(client.pair("000000")).rejects.toBeInstanceOf(LinkProtocolError);
    expect(client.state).toBe("awaiting_pairing");
    client.disconnect();
  });

  it("pairs with any PIN on an open-access hub, just to learn the session id", async () => {
    const hub = new FakeHub({ auth: "none", session: 3, role: "admin", devices: [DEVICE] });
    const client = newClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Bench" });

    expect(client.state).toBe("ready");
    expect(client.session).toBe(3);
    expect(client.role).toBe("admin");
    expect(client.token).toBeNull();
    expect(hub.received.map((m) => m.t)).toEqual(["hello", "auth.pair", "device.list"]);
    client.disconnect();
  });
});

describe("commands", () => {
  it("correlates responses by tag, even out of order", async () => {
    const pending: Array<{ tag: number; bRequest: number }> = [];
    const hub = new FakeHub({
      auth: "none",
      devices: [DEVICE],
      onCommand: (frame) => {
        pending.push({ tag: frame.tag, bRequest: frame.bRequest });
        return null; // answered by hand below, deliberately reversed
      },
    });
    const client = newClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });

    const first = client.get(0, 0x7f, 6);
    const second = client.get(0, 0xd3, 4);
    await settle();
    expect(pending.length).toBe(2);
    expect(pending[0].tag).not.toBe(pending[1].tag);

    hub.socket.deliverFrame({
      type: "cmd_response",
      tag: pending[1].tag,
      status: LinkStatus.OK,
      payload: new Uint8Array([0x00, 0x00, 0xc8, 0xc1]),
    });
    hub.socket.deliverFrame({
      type: "cmd_response",
      tag: pending[0].tag,
      status: LinkStatus.OK,
      payload: new Uint8Array([1, 1, 0x17, 9, 1, 7]),
    });

    const [platform, volume] = await Promise.all([first, second]);
    expect([...platform.payload]).toEqual([1, 1, 0x17, 9, 1, 7]);
    expect([...volume.payload]).toEqual([0x00, 0x00, 0xc8, 0xc1]);
    client.disconnect();
  });

  it("sends the spec 11.1 master volume frame", async () => {
    const hub = new FakeHub({ auth: "none", devices: [DEVICE] });
    const client = newClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });

    const payload = new Uint8Array(4);
    new DataView(payload.buffer).setFloat32(0, -20, true);
    await client.set(0, 0xd2, payload);

    const sent = hub.receivedFrames.find((f) => f.type === "cmd_request");
    expect(sent).toMatchObject({
      handle: 0,
      direction: "set",
      bRequest: 0xd2,
      wValue: 0,
      wIndex: 2,
      wLength: 4,
    });
    client.disconnect();
  });

  it("throws LinkCommandError from commandOk on a non-OK status", async () => {
    const hub = new FakeHub({
      auth: "none",
      onCommand: (f) => ({
        type: "cmd_response",
        tag: f.tag,
        status: LinkStatus.DENIED,
        payload: new Uint8Array(0),
      }),
    });
    const client = newClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });
    await expect(client.commandOk({ handle: 0, direction: "get", bRequest: 0x7f, wValue: 0, wIndex: 2, wLength: 6 }))
      .rejects.toBeInstanceOf(LinkCommandError);
    client.disconnect();
  });
});

describe("events and inventory", () => {
  it("tracks device.added, device.changed and device.removed", async () => {
    const hub = new FakeHub({ auth: "none", devices: [DEVICE] });
    const client = newClient(hub);
    const seen: number[] = [];
    client.on("devices", (list) => seen.push(list.length));
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });

    hub.socket.deliverText({ t: "device.added", device: { ...DEVICE, handle: 1, name: "Studio" } });
    await settle();
    expect(client.deviceList.map((d) => d.handle)).toEqual([0, 1]);

    hub.socket.deliverText({ t: "device.changed", device: { ...DEVICE, handle: 1, name: "Renamed" } });
    await settle();
    expect(client.device(1)?.name).toBe("Renamed");

    hub.socket.deliverText({ t: "device.removed", handle: 0, serial: DEVICE.serial });
    await settle();
    expect(client.deviceList.map((d) => d.handle)).toEqual([1]);
    expect(seen.length).toBeGreaterThanOrEqual(4);
    client.disconnect();
  });

  it("emits notify, poll and resync frames", async () => {
    const hub = new FakeHub({ auth: "none", devices: [DEVICE] });
    const client = newClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });

    let notifyOrigin = -1;
    let pollSlot = -1;
    let resyncReason = -1;
    client.on("notify", (f) => (notifyOrigin = f.origin));
    client.on("poll", (f) => (pollSlot = f.slot));
    client.on("resync", (f) => (resyncReason = f.reason));

    hub.socket.deliverBytes(fixtureBytes("frames", "notify_param_changed.bin"));
    hub.socket.deliverBytes(fixtureBytes("frames", "poll_status.bin"));
    hub.socket.deliverBytes(fixtureBytes("frames", "resync_reattached.bin"));
    await settle();

    expect(notifyOrigin).toBe(12);
    expect(pollSlot).toBe(2);
    expect(resyncReason).toBe(1);
    client.disconnect();
  });

  it("ignores a binary frame of an unknown type", async () => {
    const hub = new FakeHub({ auth: "none" });
    const client = newClient(hub);
    const errors: Error[] = [];
    client.on("error", (e) => errors.push(e));
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });

    hub.socket.deliverBytes(new Uint8Array([0x7f, 0, 0, 0, 1, 2, 3]));
    await settle();
    expect(errors).toEqual([]);
    client.disconnect();
  });
});

describe("snapshot and polls", () => {
  it("decodes the base64 blobs a snapshot carries", async () => {
    const bulk = fixtureBytes("device", "bulk_v32.bin");
    const status = fixtureBytes("device", "status_wvalue9.bin");
    const hub = new FakeHub({
      auth: "none",
      devices: [DEVICE],
      snapshot: {
        wire_version: 32,
        age_ms: 120,
        bulk_b64: bytesToBase64(bulk),
        status_b64: bytesToBase64(status),
      },
    });
    const client = newClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });

    const snap = await client.snapshot(0);
    expect(snap.wire_version).toBe(32);
    expect(snap.age_ms).toBe(120);
    expect(snap.bulk?.length).toBe(bulk.length);
    expect(snap.status?.length).toBe(status.length);
    client.disconnect();
  });

  it("returns the granted rates from poll.subscribe", async () => {
    const hub = new FakeHub({ auth: "none", devices: [DEVICE] });
    const client = newClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });

    const granted = await client.subscribePolls(0, [
      { slot: 0, req: 0x50, val: 9, idx: 2, len: 64, hz: 10 },
    ]);
    expect(granted).toEqual([{ slot: 0, hz: 10 }]);
    await client.unsubscribePolls(0, [0]);
    client.disconnect();
  });

  it("takes and releases a lock", async () => {
    const hub = new FakeHub({ auth: "none", devices: [DEVICE] });
    const client = newClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });
    await client.acquireLock(0, "Applying configuration", 10000);
    const acquire = hub.received.find((m) => m.t === "lock.acquire");
    expect(acquire).toMatchObject({ handle: 0, reason: "Applying configuration", timeout_ms: 10000 });
    await client.releaseLock(0);
    client.disconnect();
  });
});

describe("hub administration (spec 7.10 to 7.13)", () => {
  /** A bridge-like hub that answers the admin messages from the fixtures. */
  function adminHub(onRequest: (msg: Record<string, unknown>) => Record<string, unknown> | null) {
    return new FakeHub({ auth: "none", role: "admin", onRequest });
  }

  it("reads the configuration and its options", async () => {
    const reply = fixtureJson<Record<string, unknown>>("messages", "hub_config_get_ok.json");
    const hub = adminHub((msg) => (msg.t === "hub.config.get" ? reply : null));
    const client = newClient(hub);
    await client.connect("ws://hub.local:11915/dspi/v1", { clientName: "Test" });

    const { config, options } = await client.getHubConfig();
    expect(config.name).toBe("DSPi Bridge 3F2A");
    expect((options.uart as { baud: number[] }).baud).toContain(921600);
    client.disconnect();
  });

  it("names the rejected key of a hub.config.set", async () => {
    const reply = fixtureJson<Record<string, unknown>>("messages", "err_bad_field.json");
    const hub = adminHub((msg) => (msg.t === "hub.config.set" ? reply : null));
    const client = newClient(hub);
    await client.connect("ws://hub.local:11915/dspi/v1", { clientName: "Test" });

    const error = await client.setHubConfig({ uart: { tx_pin: 3 } }).catch((e) => e);
    expect(error).toBeInstanceOf(LinkProtocolError);
    expect(error.code).toBe("bad_request");
    expect(error.field).toBe("uart.tx_pin");
    expect(error.detail).toBe("pin 3 cannot be used");
    expect(hub.received.at(-1)).toMatchObject({ t: "hub.config.set", config: { uart: { tx_pin: 3 } } });
    client.disconnect();
  });

  it("emits hubConfig when the hub reports a change", async () => {
    const hub = adminHub(() => null);
    const client = newClient(hub);
    await client.connect("ws://hub.local:11915/dspi/v1", { clientName: "Test" });
    const seen: unknown[] = [];
    client.on("hubConfig", (config) => seen.push(config));

    hub.socket.deliverText(fixtureJson("messages", "hub_config_changed.json"));
    await settle();
    expect(seen).toHaveLength(1);
    expect((seen[0] as { uart: { baud: number } }).uart.baud).toBe(921600);
    client.disconnect();
  });

  it("keeps the fresh token from hub.auth.set, and drops it for mode none", async () => {
    const tokenReply = fixtureJson<Record<string, unknown>>("messages", "hub_auth_set_ok.json");
    const hub = adminHub((msg) => (msg.mode === "pin" ? tokenReply : { t: "ok" }));
    const client = newClient(hub);
    await client.connect("ws://hub.local:11915/dspi/v1", { clientName: "Test" });

    const token = await client.setAuth("pin", "482913");
    expect(token).toBe(tokenReply.token);
    expect(client.token).toBe(tokenReply.token);
    expect(hub.received.at(-1)).toMatchObject({ t: "hub.auth.set", mode: "pin", pin: "482913" });

    expect(await client.setAuth("none")).toBeNull();
    expect(client.token).toBeNull();
    expect(hub.received.at(-1)).not.toHaveProperty("pin");
    client.disconnect();
  });

  it("scans, lists clients, and sends join, revoke, restart and reset", async () => {
    const replies: Record<string, Record<string, unknown>> = {
      "hub.wifi.scan": fixtureJson("messages", "hub_wifi_scan_ok.json"),
      "auth.list": fixtureJson("messages", "auth_list_ok.json"),
    };
    const hub = adminHub((msg) => replies[msg.t as string] ?? { t: "ok" });
    const client = newClient(hub);
    await client.connect("ws://hub.local:11915/dspi/v1", { clientName: "Test" });

    const scan = await client.scanWifi();
    expect(scan.current?.ssid).toBe("Home");
    expect(scan.networks.map((n) => n.ssid)).toEqual(["Home", "Studio", "Cafe"]);
    expect((await client.listClients()).map((c) => c.cid)).toEqual([1, 2]);

    await client.joinWifi("Studio", "correct horse battery");
    await client.revokeClient(2);
    await client.restartHub();
    await client.resetHub("network");
    expect(hub.received.slice(-4)).toMatchObject([
      { t: "hub.wifi.join", ssid: "Studio", password: "correct horse battery" },
      { t: "auth.revoke", cid: 2 },
      { t: "hub.restart" },
      { t: "hub.reset", level: "network" },
    ]);
    client.disconnect();
  });
});

describe("close handling", () => {
  it("maps the close codes of spec 5", () => {
    expect(closeReasonFor(1000)).toBe("normal");
    expect(closeReasonFor(4000)).toBe("protocol_error");
    expect(closeReasonFor(4001)).toBe("auth_failed");
    expect(closeReasonFor(4002)).toBe("revoked");
    expect(closeReasonFor(4003)).toBe("shutting_down");
    expect(closeReasonFor(1006)).toBe("abnormal");
  });

  it("drops the token and does not reconnect when revoked", async () => {
    const hub = new FakeHub({ auth: "pin", validToken: "GOOD" });
    const client = new LinkClient({ webSocket: hub.webSocket, pingIntervalMs: 0, reconnect: true });
    await client.connect("ws://hub/dspi/v1", { clientName: "Test", token: "GOOD" });

    let info: { reason: string; willReconnect: boolean } | null = null;
    client.on("close", (c) => (info = c));
    hub.socket.serverClose(4002);
    await settle();

    expect(info).toMatchObject({ reason: "revoked", willReconnect: false });
    expect(client.token).toBeNull();
    expect(client.state).toBe("closed");
    client.disconnect();
  });

  it("plans a reconnect when the hub is shutting down", async () => {
    const hub = new FakeHub({ auth: "none" });
    const client = new LinkClient({
      webSocket: hub.webSocket,
      pingIntervalMs: 0,
      reconnect: true,
      reconnectMaxMs: 5,
    });
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });

    let info: { reason: string; willReconnect: boolean } | null = null;
    client.on("close", (c) => (info = c));
    hub.socket.serverClose(4003);
    await settle();

    expect(info).toMatchObject({ reason: "shutting_down", willReconnect: true });
    client.disconnect();
  });

  it("fails outstanding work when the socket goes away", async () => {
    const hub = new FakeHub({ auth: "none", onCommand: () => null });
    const client = newClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });

    const inflight = client.get(0, 0x7f, 6);
    await settle(2);
    hub.socket.serverClose(1006);
    await expect(inflight).rejects.toThrow(/closed/);
    client.disconnect();
  });
});
