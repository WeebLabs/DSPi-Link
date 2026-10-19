// Walks the hub half of the Link spec 12 conformance checklist against a
// live bridge, as part of the bench tests of bridge spec 17. It covers what
// tools/link_smoke.mjs does not: discovery, the sub-protocol, the first
// message, max_inflight and ordering, poll fan-out and stop, device events
// and handles, close codes and a reconnect storm.
//
// Usage, with the bridge on the network and its DSPi connected:
//   node tools/link_conformance.mjs <host> [--offline]
//
// --offline also moves the bridge's UART to a wrong pin for about 70 s, to
// see the device go offline, be removed and come back on the same handle,
// then puts the pin back. The DSPi itself is not touched. DNS-SD is checked
// with macOS's dns-sd tool, and skipped where it is missing.
//
// Exits non-zero if any check fails.

import { spawn } from "node:child_process";
import { LinkClient, decodeBulkHeader } from "../../../js/packages/dspi-link/dist/index.js";

const args = process.argv.slice(2);
const host = args.find((arg) => !arg.startsWith("--"));
if (!host) {
  console.error("usage: node tools/link_conformance.mjs <host> [--offline]");
  process.exit(2);
}
const withOffline = args.includes("--offline");
const base = `${host}:11915`;
const url = `ws://${base}/dspi/v1`;

const REQ_GET_STATUS = 0x50;
const REQ_GET_ALL_PARAMS = 0xa0; // The bulk read: about 60 ms at 1 Mbaud
const STATUS_OK = 0x00;
const STATUS_RATE_LIMITED = 0x84;
const DEVICE_REMOVED_WITHIN_MS = 75000; // 60 s offline retention plus margin

let failures = 0;
function check(condition, what) {
  console.log(`${condition ? "  ok  " : "  FAIL"} ${what}`);
  if (!condition) failures++;
}
const section = (title) => console.log(`\n${title}`);
const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function connect(name) {
  const client = new LinkClient({ pingIntervalMs: 0 });
  await client.connect(url, { clientName: name });
  return client;
}

/** The bridge's stats: free heap, open sessions, and every request the
 * bridge has sent its DSPi, polls included. */
async function stats(client) {
  const ok = await client.hubStats();
  return {
    heap: ok.bridge?.heap_free ?? 0,
    sessions: ok.sessions ?? 0,
    requests: ok.bridge?.uart?.requests ?? 0,
  };
}

/** Runs `command` and returns what it printed, stopping it after `ms`. */
function runFor(command, commandArgs, ms) {
  return new Promise((resolve) => {
    let output = "";
    let child;
    try {
      child = spawn(command, commandArgs);
    } catch {
      resolve(null);
      return;
    }
    child.on("error", () => resolve(null));
    child.stdout.on("data", (chunk) => (output += chunk));
    setTimeout(() => {
      child.kill();
      resolve(output);
    }, ms);
  });
}

/** Opens a raw WebSocket and resolves with what happened first. */
function rawSocket(protocols, firstText) {
  return new Promise((resolve) => {
    const ws = protocols ? new WebSocket(url, protocols) : new WebSocket(url);
    const done = (result) => {
      clearTimeout(timer);
      resolve(result);
      try {
        ws.close();
      } catch {}
    };
    const timer = setTimeout(() => done({ event: "timeout" }), 5000);
    ws.onopen = () => {
      if (firstText !== undefined) ws.send(firstText);
    };
    ws.onmessage = (event) => done({ event: "message", protocol: ws.protocol, data: event.data });
    ws.onclose = (event) => done({ event: "close", code: event.code });
    // An error is always followed by a close, which carries the code.
  });
}

async function discovery() {
  section("Discovery (Link spec 3)");
  const response = await fetch(`http://${base}/dspi/v1/info`);
  const info = await response.json();
  check(response.headers.get("access-control-allow-origin") === "*", "info allows any origin");
  check(info.proto?.major === 1 && info.ws === "/dspi/v1" && info.tls === false, "info: proto, ws, tls");
  check(/^[0-9a-f-]{36}$/.test(info.hub?.id ?? "") && info.hub?.kind === "bridge", `info: hub ${info.hub?.name}`);

  const lookup = await runFor("dns-sd", ["-L", info.hub.name, "_dspi._tcp", "local."], 3000);
  if (lookup === null) {
    console.log("  skip DNS-SD: no dns-sd tool here");
    return info;
  }
  const keys = Object.fromEntries([...lookup.matchAll(/(\w+)=([^\s]*)/g)].map((m) => [m[1], m[2]]));
  check(lookup.includes(":11915"), "DNS-SD: the service is on port 11915");
  check(keys.v === "1" && keys.hid === info.hub.id && keys.kind === "bridge", "DNS-SD TXT: v, hid, kind");
  check(keys.auth === info.auth && keys.n === String(info.devices.length), `DNS-SD TXT: auth=${keys.auth}, n=${keys.n}`);
  return info;
}

async function handshake() {
  section("Sub-protocol and first message (Link spec 4, 5)");
  const without = await rawSocket(undefined, undefined);
  check(without.event !== "message", `no sub-protocol is refused (${without.event} ${without.code ?? ""})`);

  const hello = JSON.stringify({ t: "hello", proto: { major: 1, minor: 2 }, client: { name: "conformance" } });
  const first = await rawSocket("dspi-link-1", hello);
  const message = first.event === "message" ? JSON.parse(first.data) : {};
  check(first.protocol === "dspi-link-1", "the hub accepts dspi-link-1");
  check(message.t === "hello", "the hub's first message is hello");
  console.log(`       auth is "${message.auth}"${message.auth === "pin" ? ` (${message.pin_source})` : ""}`);

  await wait(300); // Let the sockets above free their slots
  const early = await rawSocket("dspi-link-1", JSON.stringify({ t: "device.list", id: 1 }));
  check(early.event === "close" && early.code === 4000, `a request before hello closes with 4000 (${early.code})`);
}

async function commands(client) {
  section("Commands: max_inflight and ordering (Link spec 8.2)");
  // Bulk reads are slow enough that all of them are still outstanding when
  // the last arrives; quick reads can finish before the next is even read.
  const limit = client.limits.max_inflight;
  const count = limit + 4;
  const arrivals = [];
  const replies = await Promise.all(
    Array.from({ length: count }, (_, i) =>
      client
        .command({ handle: 0, direction: "get", bRequest: REQ_GET_ALL_PARAMS, wValue: 0, wIndex: 0, wLength: 6136 })
        .then((reply) => {
          arrivals.push(i);
          return reply.status;
        }),
    ),
  );
  const ok = replies.filter((status) => status === STATUS_OK).length;
  const limited = replies.filter((status) => status === STATUS_RATE_LIMITED).length;
  check(ok >= limit && limited > 0 && ok + limited === count,
        `${count} at once with max_inflight ${limit}: ${ok} OK, ${limited} RATE_LIMITED`);
  const answered = arrivals.filter((i) => replies[i] === STATUS_OK);
  check(answered.every((value, i) => i === 0 || value > answered[i - 1]), "answers come in the order sent");
}

async function polls(clients, statusLength) {
  section("Polls: fan-out and stop (Link spec 7.6)");
  const [first, second] = clients;
  const poll = { slot: 2, req: REQ_GET_STATUS, val: 9, idx: 2, len: statusLength, hz: 10 };
  const received = [0, 0];
  const stops = clients.map((client, i) => client.on("poll", () => received[i]++));
  await first.subscribePolls(0, [poll]);
  await second.subscribePolls(0, [poll]);
  const before = await stats(first);
  await wait(3000);
  const during = await stats(first);
  const perSecond = (during.requests - before.requests) / 3;
  check(received[0] >= 20 && received[1] >= 20, `both sessions receive the poll (${received[0]}, ${received[1]})`);
  check(perSecond > 5 && perSecond < 15, `the DSPi is read once for both: ${perSecond.toFixed(1)} reads a second at 10 Hz`);

  await first.unsubscribePolls(0, [2]);
  await second.unsubscribePolls(0, [2]);
  await wait(500);
  const stopped = await stats(first);
  await wait(3000);
  const after = await stats(first);
  // The bridge's own heartbeat reads the DSPi now and then; polls would be 30 in 3 s.
  check(after.requests - stopped.requests < 8, `polling stops when nobody subscribes (${after.requests - stopped.requests} reads in 3 s)`);
  stops.forEach((stop) => stop());
}

/** Waits for a device event `t` on `client`, or rejects after `ms`. */
function nextDeviceEvent(client, t, ms) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      stop();
      reject(new Error(`no ${t} within ${ms} ms`));
    }, ms);
    const stop = client.on("message", (message) => {
      if (message.t === t) {
        clearTimeout(timer);
        stop();
        resolve(message);
      }
    });
  });
}

async function offlineAndBack(client) {
  section("Device events and handles (Link spec 7.4)");
  const { config, options } = await client.getHubConfig();
  const original = config.uart;
  const wrongPin = options.uart.tx_pin.find((pin) => pin !== original.tx_pin && pin !== original.rx_pin);
  const serial = client.deviceList[0]?.serial;

  const offline = nextDeviceEvent(client, "device.changed", 15000);
  await client.setHubConfig({ uart: { tx_pin: wrongPin, detect_baud: false } });
  const changed = await offline.catch(() => null);
  check(changed?.device?.state === "offline", `moving the UART to GPIO ${wrongPin}: the device goes offline`);

  const removed = await nextDeviceEvent(client, "device.removed", DEVICE_REMOVED_WITHIN_MS).catch(() => null);
  check(removed?.handle === 0, "after 60 s offline the device is removed");

  const added = nextDeviceEvent(client, "device.added", 20000);
  await client.setHubConfig({ uart: original });
  const back = await added.catch(() => null);
  check(back?.device?.handle === 0 && back?.device?.serial === serial,
        `pins restored: device.added on handle 0 with the same serial (${back?.device?.serial})`);
}

/** Connects, retrying a refusal: a closed session keeps its slot until the
 * hub has seen it close (bridge spec 6.2), which takes a few tens of ms. */
async function connectWithRetry(name, onRefused) {
  for (let attempt = 0; ; attempt++) {
    try {
      return await connect(name);
    } catch (error) {
      if (attempt >= 10) throw error;
      onRefused();
      await wait(100);
    }
  }
}

async function storm(client) {
  section("Close codes and a reconnect storm (Link spec 5)");
  const before = await stats(client);
  let briefRefusals = 0;
  for (let i = 0; i < 40; i++) {
    const other = await connectWithRetry(`storm ${i}`, () => briefRefusals++);
    other.disconnect();
  }
  check(true, `40 connections back to back, none lost (${briefRefusals} refused while slots were freed)`);
  await wait(1000);
  const extra = await Promise.allSettled(Array.from({ length: 12 }, (_, i) => connect(`burst ${i}`)));
  const opened = extra.filter((result) => result.status === "fulfilled").map((result) => result.value);
  const refused = extra.length - opened.length;
  check(opened.length > 0 && refused > 0, `12 at once: ${opened.length} opened, ${refused} refused while full`);
  opened.forEach((other) => other.disconnect());
  await wait(3000);
  const after = await stats(client);
  check(after.sessions === before.sessions, `back to ${before.sessions} session(s) afterwards (${after.sessions})`);
  check(Math.abs(after.heap - before.heap) < 16384,
        `free heap before ${before.heap}, after ${after.heap}`);
}

async function main() {
  const info = await discovery();
  await handshake();

  const client = await connect("conformance");
  const listener = await connect("conformance listener");
  check(client.deviceList.length === 1, `the DSPi is online (${info.devices[0]?.serial})`);
  await commands(client);
  // The combined status frame's length, as tools/link_smoke.mjs works it out.
  const snapshot = await client.snapshot(0);
  await polls([client, listener], 2 * decodeBulkHeader(snapshot.bulk).numChannels + 7);
  listener.disconnect();
  if (withOffline) await offlineAndBack(client);
  await storm(client);

  client.disconnect();
  console.log(failures === 0 ? "\nAll checks passed." : `\n${failures} check(s) failed.`);
  process.exit(failures === 0 ? 0 : 1);
}

main().catch((error) => {
  console.error("FAILED:", error);
  process.exit(1);
});
