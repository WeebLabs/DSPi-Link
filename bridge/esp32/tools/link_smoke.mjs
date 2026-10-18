#!/usr/bin/env node
/*
 * link_smoke.mjs - drives a live hub through the main Link features with the
 * repository's own client library (dspi-link-js), as a quick end-to-end
 * check of a bridge on the bench (bridge spec 17).
 *
 * Usage, after `npm run build` in js/:
 *   node tools/link_smoke.mjs <host[:port]>
 *
 * It opens two sessions, lists the device, reads the platform, takes a
 * snapshot, lowers the master volume by 0.5 dB and checks that both sessions
 * hear the notification with the writer's session as origin, restores the
 * volume, polls the meters for two seconds, and takes and releases the
 * lock. The DSPi ends as it started. Exits non-zero if any check fails.
 */

import {
  LinkClient,
  decodePlatform,
  readMasterVolumeDb,
  decodeBulkHeader,
  decodeNotifyPacket,
} from "../../../js/packages/dspi-link/dist/index.js";

const host = process.argv[2];
if (!host) {
  console.error("usage: node tools/link_smoke.mjs <host[:port]>");
  process.exit(2);
}
const url = `ws://${host.includes(":") ? host : `${host}:11915`}/dspi/v1`;

const REQ_GET_PLATFORM = 0x7f;
const REQ_GET_STATUS = 0x50;
const REQ_SET_MASTER_VOLUME = 0xd2;

let failures = 0;
function check(condition, what) {
  console.log(`${condition ? "  ok  " : "  FAIL"} ${what}`);
  if (!condition) failures++;
}

const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function main() {
  const writer = new LinkClient();
  const listener = new LinkClient();
  await writer.connect(url, { clientName: "link_smoke writer" });
  await listener.connect(url, { clientName: "link_smoke listener" });
  check(writer.state === "ready" && listener.state === "ready", "both sessions are ready");
  check(writer.hub?.hub?.kind === "bridge", `hub kind is bridge (${writer.hub?.hub?.name})`);
  check(writer.session !== null && writer.session !== listener.session, "distinct session ids");

  const devices = await writer.refreshDevices();
  check(devices.length === 1, `one device listed (${devices[0]?.serial}, fw ${devices[0]?.fw})`);
  const handle = devices[0].handle;

  const platformBytes = await writer.commandOk({
    handle, direction: "get", bRequest: REQ_GET_PLATFORM, wValue: 0, wIndex: 2, wLength: 7,
  });
  const platform = decodePlatform(platformBytes);
  check(platformBytes.length >= 6, `platform read (${JSON.stringify(platform)})`);

  const started = Date.now();
  const snapshot = await writer.snapshot(handle);
  const header = decodeBulkHeader(snapshot.bulk);
  const volumeDb = readMasterVolumeDb(snapshot.bulk);
  check(snapshot.bulk.length >= 6000,
        `snapshot: ${snapshot.bulk.length} bytes, wire V${header.wireVersion}, ` +
        `${volumeDb.toFixed(1)} dB, age ${snapshot.age_ms} ms, ${Date.now() - started} ms`);

  // Nudge the volume and watch both sessions hear it. The DSPi announces
  // only real changes, so writing the same value would stay silent.
  const heard = { writer: null, listener: null };
  writer.on("notify", (frame) => { heard.writer ??= frame; });
  listener.on("notify", (frame) => { heard.listener ??= frame; });
  const asBytes = (db) => new Uint8Array(new Float32Array([db]).buffer);
  const setVolume = (client, db) => client.command({
    handle, direction: "set", bRequest: REQ_SET_MASTER_VOLUME, wValue: 0, wIndex: 2,
    payload: asBytes(db),
  });
  await writer.commandOk({
    handle, direction: "set", bRequest: REQ_SET_MASTER_VOLUME, wValue: 0, wIndex: 2,
    payload: asBytes(volumeDb - 0.5),
  });
  await wait(500);
  await setVolume(writer, volumeDb); // Restore
  check(heard.listener !== null, "the other session heard the notification");
  if (heard.listener) {
    const packet = decodeNotifyPacket(heard.listener.packet);
    check(heard.listener.origin === writer.session,
          `origin is the writer's session (${heard.listener.origin}), event ${packet?.event}`);
  }
  check(heard.writer?.origin === writer.session, "the writer can recognise its own echo");

  const statusLength = 2 * header.numChannels + 7;
  let polls = 0;
  listener.on("poll", () => { polls++; });
  const grants = await listener.subscribePolls(handle, [
    { slot: 3, req: REQ_GET_STATUS, val: 9, idx: 2, len: statusLength, hz: 10 },
  ]);
  await wait(2000);
  await listener.unsubscribePolls(handle, [3]);
  check(polls >= 15, `meter polls at ${grants[0]?.hz} Hz: ${polls} frames in 2 s`);

  await writer.acquireLock(handle, "link_smoke", 5000);
  const blocked = await setVolume(listener, volumeDb);
  check(blocked.status === 0x85, `a write from another session is LOCKED (0x${blocked.status.toString(16)})`);
  await writer.releaseLock(handle);

  const stats = await writer.request({ t: "hub.stats", id: 0 });
  check(typeof stats.uptime_s === "number", `hub.stats: ${JSON.stringify(stats.bridge?.uart)}`);

  writer.disconnect();
  listener.disconnect();
  console.log(failures === 0 ? "All checks passed." : `${failures} check(s) failed.`);
  process.exit(failures === 0 ? 0 : 1);
}

main().catch((error) => {
  console.error("FAILED:", error);
  process.exit(1);
});
