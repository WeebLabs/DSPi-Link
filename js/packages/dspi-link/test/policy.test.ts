/*
 * Structural checks on policy/commands.json (Link spec 9.3). Every hub embeds
 * this file, so a malformed row is an authorization bug in every implementation.
 * The bridge's tools/gen_policy.py --check also compares it with the firmware
 * registry in policy/firmware/.
 */

import { readFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { FIXTURES } from "./fixtures.js";

type Dir = "set" | "get";
type Cls = "read" | "control" | "config";

interface Row {
  id: number;
  name: string;
  classes: Partial<Record<Dir, Cls>>;
  note?: string;
}

const table = JSON.parse(readFileSync(join(FIXTURES, "..", "policy", "commands.json"), "utf8")) as {
  spec_version: string;
  commands: Row[];
};

function byName(name: string): Row {
  const row = table.commands.find((r) => r.name === name);
  if (!row) throw new Error(`${name} is not in the table`);
  return row;
}

describe("policy table", () => {
  it("is spec 1.3", () => {
    expect(table.spec_version).toBe("1.3");
  });

  it("lists each command ID once, in ascending order, with a page above 0", () => {
    const ids = table.commands.map((r) => r.id);
    expect(ids.every((id) => Number.isInteger(id) && id >= 0x0100 && id <= 0xffff)).toBe(true);
    for (let i = 1; i < ids.length; i++) expect(ids[i]).toBeGreaterThan(ids[i - 1]);
  });

  it("gives every row a PAGE.NAME, at least one direction and valid classes", () => {
    for (const row of table.commands) {
      expect(row.name, `id ${row.id}`).toMatch(/^[A-Z]+\.[A-Z0-9_]+$/);
      const dirs = Object.keys(row.classes);
      expect(dirs.length, row.name).toBeGreaterThan(0);
      for (const d of dirs) expect(["set", "get"], row.name).toContain(d);
      for (const c of Object.values(row.classes)) {
        expect(["read", "control", "config"], row.name).toContain(c);
      }
    }
  });

  it("classes every GET as read, since a GET never changes the device", () => {
    for (const row of table.commands.filter((r) => r.classes.get)) {
      expect(row.classes.get, row.name).toBe("read");
    }
  });

  it("never classes a SET as read, so a viewer can never write", () => {
    for (const row of table.commands.filter((r) => r.classes.set)) {
      expect(row.classes.set, row.name).not.toBe("read");
    }
  });

  it("lets a viewer read the limiter but not change it", () => {
    const threshold = byName("DYN.LIMITER_THRESHOLD");
    expect(threshold.id).toBe(0x0711);
    expect(threshold.classes).toEqual({ get: "read", set: "control" });
  });

  // Each of these shares its command number with a control-class 1.1.6
  // opcode, so a table keyed by bRequest alone would let any control
  // session run it (firmware migration guide 13.3.8).
  it.each([
    ["SYS.ENTER_BOOTLOADER", 0x0130],
    ["STORE.FACTORY_RESET", 0x0230],
    ["STORE.STARTUP", 0x0216],
    ["INPUT.I2S_RX_PIN", 0x0330],
    ["INPUT.I2S_RX_RATE", 0x0332],
    ["INPUT.ADAT_RX_CLOCK_MODE", 0x0342],
    ["OUTPUT.TYPE", 0x0410],
    ["OUTPUT.ADAT_TX_ENABLE", 0x0430],
    ["CS.SAVE", 0x0a18],
    ["CS.IR_CMD", 0x0a30],
    ["CS.IR_LEARN_ARM", 0x0a32],
    ["CS.DISPLAY_CONFIG", 0x0a70],
    ["XPORT.UART_CONFIG", 0x0b10],
    ["CLOCK.I2S_BCK_PIN", 0x0c10],
    ["CLOCK.I2S_CLOCK_MODE", 0x0c12],
  ])("keeps %s admin-only", (name, id) => {
    const row = byName(name);
    expect(row.id).toBe(id);
    expect(row.classes.set).toBe("config");
  });

  it("lets a control user send what Link's own clients send", () => {
    for (const name of [
      "ROUTE.MASTER_VOLUME",
      "SYS.DEVICE_NAME",
      "STORE.ALL_PARAMS",
      "MEAS.RTA_CONFIG",
      "MEAS.RTA_START",
      "MEAS.RTA_STOP",
    ]) {
      expect(byName(name).classes.set, name).toBe("control");
    }
  });
});
