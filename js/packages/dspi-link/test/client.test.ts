/*
 * LinkClient against the in-process FakeHub: the handshake, correlation,
 * events, polls, administration, firmware upload and close handling of Link
 * spec 5 to 8, and the races and timeouts of a session that goes wrong.
 */

import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { LinkClient, closeReasonFor } from "../src/client.js";
import { LINK_SUBPROTOCOL } from "../src/constants.js";
import { LinkCommandError, LinkProtocolError } from "../src/errors.js";
import { LinkStatus, ResyncReason, type LinkFrame } from "../src/frames.js";
import { bytesToBase64 } from "../src/base64.js";
import type { DeviceInfo } from "../src/messages.js";
import { FakeHub, settle, waitFor, type FakeHubConfig } from "./fakeHub.js";
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

/** A client that reconnects at once, for the tests that drop the socket. */
function newReconnectingClient(hub: FakeHub): LinkClient {
  return new LinkClient({
    webSocket: hub.webSocket,
    pingIntervalMs: 0,
    reconnect: true,
    reconnectMaxMs: 5,
  });
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

describe("hub firmware update (spec 7.14)", () => {
  /** A hub that answers hub.update with transfer 5. */
  function updateHub(onFwData?: FakeHubConfig["onFwData"]) {
    return new FakeHub({
      auth: "none",
      role: "admin",
      onRequest: (msg) => (msg.t === "hub.update" ? { t: "ok", xfer: 5 } : null),
      onFwData,
    });
  }

  it("sends the image in order, a few chunks ahead, and returns the new version", async () => {
    const hub = updateHub();
    const client = newClient(hub);
    await client.connect("ws://hub.local:11915/dspi/v1", { clientName: "Test" });
    const image = Uint8Array.from({ length: 150000 }, (_, i) => i & 0xff);
    const progress: number[] = [];

    const update = client.updateHub(image, {
      version: "0.2.0",
      onProgress: (written) => progress.push(written),
    });
    // The fake hub acknowledges each chunk at once; finish once all are in.
    await settle(40);
    hub.socket.deliverText({ t: "hub.update.done", ok: true, version: "0.2.0" });
    expect(await update).toBe("0.2.0");

    const request = hub.received.find((m) => m.t === "hub.update")!;
    expect(request.size).toBe(150000);
    expect(request.sha256).toMatch(/^[0-9a-f]{64}$/);
    const chunks = hub.receivedFrames.filter((f) => f.type === "fwdata");
    expect(chunks.map((c) => (c as { offset: number }).offset)).toEqual([0, 65528, 131056]);
    expect(chunks.every((c) => c.tag === 5)).toBe(true);
    expect(progress.at(-1)).toBe(150000);
    client.disconnect();
  });

  it("keeps no more than two chunks unanswered", async () => {
    const held: Array<{ tag: number; offset: number }> = [];
    const hub = updateHub((frame) => {
      held.push({ tag: frame.tag, offset: frame.offset });
      return null; // Answer later, by hand
    });
    const client = newClient(hub);
    await client.connect("ws://hub.local:11915/dspi/v1", { clientName: "Test" });
    const update = client.updateHub(new Uint8Array(300000));
    await settle(20);
    expect(held).toHaveLength(2);

    hub.socket.deliverFrame({ type: "fwdata_ack", tag: 5, offset: held[0].offset, status: 0 });
    await settle(20);
    expect(held).toHaveLength(3); // One answered, so one more went out

    hub.socket.deliverText({ t: "hub.update.done", ok: false, msg: "the file was damaged" });
    await expect(update).rejects.toThrow("the file was damaged");
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

describe("sessions that go wrong", () => {
  const POLL = { slot: 0, req: 0x50, val: 9, idx: 2, len: 41, hz: 10 };

  /** The listeners of `event` registered on a client, read for leak checks. */
  function listenerCount(client: LinkClient, event: string): number {
    const listeners = (client as unknown as { listeners: Map<string, Set<unknown>> }).listeners;
    return listeners.get(event)?.size ?? 0;
  }

  it("ignores a stale socket after reconnecting", async () => {
    const hub = new FakeHub({ auth: "none", devices: [DEVICE] });
    const client = new LinkClient({
      webSocket: hub.webSocket,
      pingIntervalMs: 0,
      reconnect: true,
      reconnectMaxMs: 5,
    });
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });
    const old = hub.socket;
    old.serverClose(4003);
    await waitFor(() => hub.sockets.length === 2 && client.state === "ready");

    // The old socket speaks again: a duplicate close and a late event.
    old.serverClose(1006);
    old.deliverText({ t: "device.removed", handle: 0, serial: DEVICE.serial });
    await settle();
    expect(client.state).toBe("ready");
    expect(client.deviceList).toHaveLength(1);
    await expect(client.get(0, 0x7f, 6)).resolves.toMatchObject({ status: LinkStatus.OK });
    client.disconnect();
  });

  it("replaces the old socket when connect is called again", async () => {
    const hub = new FakeHub({ auth: "none", devices: [DEVICE] });
    const client = newClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });
    const first = hub.socket;
    client.disconnect();
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });

    expect(first.readyState).toBe(3);
    first.serverClose(1006); // Its close event, arriving late
    await settle();
    expect(client.state).toBe("ready");
    expect(hub.sockets).toHaveLength(2);
    client.disconnect();
  });

  it("keeps the token when the socket closes during auth, and uses it again", async () => {
    const hub = new FakeHub({ auth: "pin", validToken: "GOOD", ignore: ["auth.token"] });
    const client = new LinkClient({
      webSocket: hub.webSocket,
      pingIntervalMs: 0,
      reconnect: true,
      reconnectMaxMs: 5,
    });
    const connecting = client.connect("ws://hub/dspi/v1", { clientName: "Test", token: "GOOD" });
    await waitFor(() => hub.received.some((message) => message.t === "auth.token"));
    hub.config.ignore = [];
    hub.socket.serverClose(1006);
    await expect(connecting).rejects.toThrow(/closed/);
    expect(client.token).toBe("GOOD");

    await waitFor(() => client.state === "ready");
    expect(hub.received.filter((message) => message.t === "auth.token")).toHaveLength(2);
    client.disconnect();
  });

  it("removes its listener when the hub refuses hub.update", async () => {
    const hub = new FakeHub({
      auth: "none",
      role: "admin",
      onRequest: (message) =>
        message.t === "hub.update" ? { t: "err", code: "busy", msg: "an update is running" } : null,
    });
    const client = newClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });
    const before = listenerCount(client, "message");

    await expect(client.updateHub(new Uint8Array(1000))).rejects.toBeInstanceOf(LinkProtocolError);
    expect(listenerCount(client, "message")).toBe(before);
    client.disconnect();
  });

  it("closes a socket that stops answering keepalives", async () => {
    const hub = new FakeHub({ auth: "none", devices: [DEVICE] });
    const client = new LinkClient({
      webSocket: hub.webSocket,
      pingIntervalMs: 20,
      requestTimeoutMs: 30,
      reconnect: false,
    });
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });
    let closed: { reason: string } | null = null;
    client.on("close", (info) => (closed = info));
    hub.config.ignore = ["ping"];

    await waitFor(() => client.state === "closed");
    expect(closed).toMatchObject({ reason: "abnormal" });
    expect(hub.socket.readyState).toBe(3);
    client.disconnect();
  });

  it("gives up on a hub that never answers hello", async () => {
    const hub = new FakeHub({ auth: "none", ignore: ["hello"] });
    const client = new LinkClient({
      webSocket: hub.webSocket,
      pingIntervalMs: 0,
      handshakeTimeoutMs: 30,
    });
    await expect(client.connect("ws://hub/dspi/v1", { clientName: "Test" })).rejects.toThrow(
      "the hub did not answer in time",
    );
    expect(client.state).toBe("closed");
    expect(hub.socket.readyState).toBe(3);
  });

  it("subscribes to polls again when a device comes back online", async () => {
    const hub = new FakeHub({ auth: "none", devices: [DEVICE] });
    const client = newClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });
    await client.subscribePolls(0, [POLL]);

    hub.socket.deliverText({ t: "device.changed", device: { ...DEVICE, state: "offline" } });
    await settle();
    hub.socket.deliverText({ t: "device.changed", device: { ...DEVICE, state: "online" } });
    await settle();

    const subscribes = hub.received.filter((message) => message.t === "poll.subscribe");
    expect(subscribes).toHaveLength(2);
    expect(subscribes[1]).toMatchObject({ handle: 0, polls: [POLL] });
    client.disconnect();
  });

  it("does not restore a subscription ended before its reply arrived", async () => {
    const hub = new FakeHub({ auth: "none", devices: [DEVICE] });
    const client = newClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });
    const subscribing = client.subscribePolls(0, [POLL]);
    const unsubscribing = client.unsubscribePolls(0, [POLL.slot]);
    await Promise.all([subscribing, unsubscribing]);

    hub.socket.deliverText({ t: "device.changed", device: { ...DEVICE, state: "offline" } });
    hub.socket.deliverText({ t: "device.changed", device: { ...DEVICE, state: "online" } });
    await settle();
    expect(hub.received.filter((message) => message.t === "poll.subscribe")).toHaveLength(1);
    client.disconnect();
  });

  it("does not reconnect after a protocol error", async () => {
    const hub = new FakeHub({ auth: "none" });
    const client = new LinkClient({ webSocket: hub.webSocket, pingIntervalMs: 0, reconnect: true });
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });
    let info: { willReconnect: boolean } | null = null;
    client.on("close", (close) => (info = close));
    hub.socket.serverClose(4000);
    await settle();
    expect(info).toMatchObject({ willReconnect: false });
    expect(client.state).toBe("closed");
  });
});

describe("missed notifications (spec 8.3)", () => {
  /** A NOTIFY frame carrying a v2 packet with firmware `seq`. */
  function notify(tag: number, handle: number, seq: number): LinkFrame {
    return { type: "notify", tag, handle, origin: 0, packet: new Uint8Array([2, 0x02, 0, seq, 0]) };
  }

  async function watch(devices: DeviceInfo[]) {
    const hub = new FakeHub({ auth: "none", devices });
    const client = newClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });
    const resyncs: Array<{ handle: number; reason: number }> = [];
    client.on("resync", (frame) => resyncs.push({ handle: frame.handle, reason: frame.reason }));
    return { hub, client, resyncs };
  }

  it("stays quiet while both sequences run on", async () => {
    const { hub, client, resyncs } = await watch([DEVICE]);
    for (const [tag, seq] of [[7, 254], [8, 255], [9, 0], [10, 1]]) {
      hub.socket.deliverFrame(notify(tag, 0, seq));
    }
    await settle();
    expect(resyncs).toEqual([]);
    client.disconnect();
  });

  it("re-reads every device after a gap in the NOTIFY tags, which count them all", async () => {
    const { hub, client, resyncs } = await watch([DEVICE, { ...DEVICE, handle: 1 }]);
    hub.socket.deliverFrame(notify(0xfffe, 0, 1));
    hub.socket.deliverFrame(notify(0xffff, 0, 2));
    hub.socket.deliverFrame(notify(0, 0, 3)); // the tag wraps: no gap
    hub.socket.deliverFrame(notify(2, 1, 9)); // tag 1 went missing
    await settle();
    expect(resyncs).toEqual([
      { handle: 0, reason: ResyncReason.NOTIFY_DROPPED },
      { handle: 1, reason: ResyncReason.NOTIFY_DROPPED },
    ]);
    client.disconnect();
  });

  it("re-reads one device after a gap in its firmware seq", async () => {
    const { hub, client, resyncs } = await watch([DEVICE, { ...DEVICE, handle: 1 }]);
    hub.socket.deliverFrame(notify(1, 0, 10));
    hub.socket.deliverFrame(notify(2, 1, 50));
    hub.socket.deliverFrame(notify(3, 0, 12)); // seq 11 never reached the hub
    hub.socket.deliverFrame(notify(4, 1, 51));
    await settle();
    expect(resyncs).toEqual([{ handle: 0, reason: ResyncReason.NOTIFY_DROPPED }]);
    client.disconnect();
  });

  it("starts the firmware seq afresh when a device comes back", async () => {
    const { hub, client, resyncs } = await watch([DEVICE]);
    hub.socket.deliverFrame(notify(1, 0, 40));
    hub.socket.deliverText({ t: "device.changed", device: { ...DEVICE, state: "offline" } });
    hub.socket.deliverText({ t: "device.changed", device: { ...DEVICE, state: "online" } });
    hub.socket.deliverFrame(notify(2, 0, 0));
    await settle();
    expect(resyncs).toEqual([]);
    client.disconnect();
  });

  it("starts the NOTIFY tags afresh on a new session", async () => {
    const hub = new FakeHub({ auth: "none", devices: [DEVICE] });
    const client = newReconnectingClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });
    const resyncs: number[] = [];
    client.on("resync", (frame) => resyncs.push(frame.handle));
    hub.socket.deliverFrame(notify(500, 0, 1));
    await settle();
    hub.socket.serverClose(4003);
    await waitFor(() => hub.sockets.length === 2 && client.state === "ready");
    hub.socket.deliverFrame(notify(0, 0, 2));
    await settle();
    expect(resyncs).toEqual([]);
    client.disconnect();
  });
});

describe("restoring polls on a new session", () => {
  const POLL = { slot: 0, req: 0x50, val: 9, idx: 2, len: 41, hz: 10 };

  /** Drops the socket and returns the poll.subscribe messages of the new session. */
  async function reconnect(
    hub: FakeHub,
    client: LinkClient,
  ): Promise<Array<Record<string, unknown>>> {
    const before = hub.received.length;
    const sockets = hub.sockets.length;
    hub.socket.serverClose(4003);
    await waitFor(() => hub.sockets.length === sockets + 1 && client.state === "ready");
    await settle();
    return hub.received.slice(before).filter((message) => message.t === "poll.subscribe");
  }

  it("forgets a device removed while disconnected, and still restores the others", async () => {
    const hub = new FakeHub({ auth: "none", devices: [DEVICE, { ...DEVICE, handle: 1 }] });
    const client = newReconnectingClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });
    await client.subscribePolls(0, [POLL]);
    await client.subscribePolls(1, [POLL]);

    hub.config.devices = [{ ...DEVICE, handle: 1 }];
    hub.config.noDevice = [0];
    expect((await reconnect(hub, client)).map((message) => message.handle)).toEqual([1]);
    expect((await reconnect(hub, client)).map((message) => message.handle)).toEqual([1]);
    client.disconnect();
  });

  it("restores the others when one device refuses, and keeps that one for later", async () => {
    const hub = new FakeHub({ auth: "none", devices: [DEVICE, { ...DEVICE, handle: 1 }] });
    const client = newReconnectingClient(hub);
    const errors: Error[] = [];
    client.on("error", (error) => errors.push(error));
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });
    await client.subscribePolls(0, [POLL]);
    await client.subscribePolls(1, [POLL]);

    hub.config.noDevice = [0];
    expect((await reconnect(hub, client)).map((message) => message.handle)).toEqual([0, 1]);
    expect(errors.some((error) => error instanceof LinkProtocolError)).toBe(true);
    hub.config.noDevice = [];
    expect((await reconnect(hub, client)).map((message) => message.handle)).toEqual([0, 1]);
    client.disconnect();
  });

  it("leaves an offline device's polls until it comes back online", async () => {
    const hub = new FakeHub({ auth: "none", devices: [DEVICE] });
    const client = newReconnectingClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });
    await client.subscribePolls(0, [POLL]);

    hub.config.devices = [{ ...DEVICE, state: "offline" }];
    expect(await reconnect(hub, client)).toEqual([]);
    hub.socket.deliverText({ t: "device.changed", device: DEVICE });
    await settle();
    expect(hub.received.at(-1)).toMatchObject({ t: "poll.subscribe", handle: 0 });
    client.disconnect();
  });
});

describe("tokens and hub ids (spec 6.2)", () => {
  const HUB_A = "hub-a";
  const HUB_B = "hub-b";

  it("looks the token up by the id in the hub's hello", async () => {
    const hub = new FakeHub({
      auth: "pin",
      validToken: "TOKEN-A",
      hubId: HUB_A,
      devices: [DEVICE],
    });
    const client = newClient(hub);
    const asked: string[] = [];
    await client.connect("ws://hub/dspi/v1", {
      clientName: "Test",
      tokenFor: (hubId) => (asked.push(hubId), hubId === HUB_A ? "TOKEN-A" : null),
    });
    expect(asked).toEqual([HUB_A]);
    expect(client.state).toBe("ready");
    client.disconnect();
  });

  it("never offers a token to another hub that answers at the same address", async () => {
    const hub = new FakeHub({
      auth: "pin",
      validToken: "TOKEN-A",
      hubId: HUB_A,
      devices: [DEVICE],
    });
    const client = newReconnectingClient(hub);
    await client.connect("ws://192.168.1.50:11915/dspi/v1", {
      clientName: "Test",
      token: "TOKEN-A",
    });
    expect(client.state).toBe("ready");

    hub.config.hubId = HUB_B;
    hub.socket.serverClose(1006);
    await waitFor(() => hub.sockets.length === 2 && client.state === "awaiting_pairing");
    expect(hub.received.filter((message) => message.t === "auth.token")).toHaveLength(1);
    expect(client.token).toBeNull();
    client.disconnect();
  });

  it("asks tokenFor again when another hub answers", async () => {
    const hub = new FakeHub({
      auth: "pin",
      validToken: "TOKEN-A",
      hubId: HUB_A,
      devices: [DEVICE],
    });
    const client = newReconnectingClient(hub);
    const stored: Record<string, string> = { [HUB_A]: "TOKEN-A", [HUB_B]: "TOKEN-B" };
    await client.connect("ws://hub/dspi/v1", {
      clientName: "Test",
      tokenFor: (hubId) => stored[hubId],
    });

    hub.config.hubId = HUB_B;
    hub.config.validToken = "TOKEN-B";
    hub.socket.serverClose(1006);
    await waitFor(() => hub.sockets.length === 2 && client.state === "ready");
    const tokens = hub.received.filter((message) => message.t === "auth.token").map((m) => m.token);
    expect(tokens).toEqual(["TOKEN-A", "TOKEN-B"]);
    client.disconnect();
  });

  it("reports a refused token, with the hub it belonged to", async () => {
    const hub = new FakeHub({ auth: "pin", validToken: "GOOD", hubId: HUB_A });
    const client = newClient(hub);
    const rejected: string[] = [];
    client.on("tokenRejected", ({ hubId }) => rejected.push(hubId));
    await client.connect("ws://hub/dspi/v1", { clientName: "Test", tokenFor: () => "STALE" });
    expect(client.state).toBe("awaiting_pairing");
    expect(rejected).toEqual([HUB_A]);
    client.disconnect();
  });

  it("reports a revoked token", async () => {
    const hub = new FakeHub({ auth: "pin", validToken: "GOOD", hubId: HUB_A, devices: [DEVICE] });
    const client = newClient(hub);
    const rejected: string[] = [];
    client.on("tokenRejected", ({ hubId }) => rejected.push(hubId));
    await client.connect("ws://hub/dspi/v1", { clientName: "Test", tokenFor: () => "GOOD" });
    hub.socket.serverClose(4002);
    await settle();
    expect(rejected).toEqual([HUB_A]);
    expect(client.token).toBeNull();
  });
});

describe("open access without a session id", () => {
  it("goes on as admin when the courtesy auth.pair gets no answer", async () => {
    const hub = new FakeHub({ auth: "none", devices: [DEVICE], ignore: ["auth.pair"] });
    const client = new LinkClient({
      webSocket: hub.webSocket,
      pingIntervalMs: 0,
      requestTimeoutMs: 30,
    });
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });
    expect(client.state).toBe("ready");
    expect(client.role).toBe("admin");
    expect(client.session).toBeNull();
    expect(client.deviceList).toEqual([DEVICE]);
    client.disconnect();
  });
});

describe("hub.auth.set and the hello", () => {
  it("keeps hub.auth and pin_source in step with the change", async () => {
    const hub = new FakeHub({
      auth: "none",
      onRequest: (message) =>
        message.t === "hub.auth.set"
          ? message.mode === "pin"
            ? { t: "ok", token: "FRESH" }
            : { t: "ok" }
          : null,
    });
    const client = newClient(hub);
    await client.connect("ws://hub/dspi/v1", { clientName: "Test" });
    expect(client.hub?.auth).toBe("none");

    await client.setAuth("pin", "123456");
    expect(client.hub?.auth).toBe("pin");
    expect(client.hub?.pin_source).toBe("fixed");

    await client.setAuth("none");
    expect(client.hub?.auth).toBe("none");
    expect(client.hub?.pin_source).toBeUndefined();
    client.disconnect();
  });
});

describe("constants", () => {
  it("names the version in package.json", async () => {
    const { LINK_LIBRARY_VERSION } = await import("../src/constants.js");
    const packageJson = JSON.parse(
      readFileSync(new URL("../package.json", import.meta.url), "utf8"),
    ) as { version: string };
    expect(LINK_LIBRARY_VERSION).toBe(packageJson.version);
  });
});
