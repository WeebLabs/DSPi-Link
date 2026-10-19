/*
 * Structural checks on policy/commands.json (Link spec 9.3). Every hub embeds
 * this file, so a malformed row is an authorization bug in every implementation.
 */

import { readFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { FIXTURES } from "./fixtures.js";

type Dir = "set" | "get";
type Cls = "read" | "control" | "config";

interface Row {
  code: number;
  name: string;
  dirs: Dir[];
  class: Cls;
  classes?: Partial<Record<Dir, Cls>>;
  note?: string;
}

const table = JSON.parse(readFileSync(join(FIXTURES, "..", "policy", "commands.json"), "utf8")) as {
  spec_version: string;
  generated: string;
  commands: Row[];
};

const RANK: Record<Cls, number> = { read: 0, control: 1, config: 2 };

function classOf(row: Row, dir: Dir): Cls {
  return row.classes?.[dir] ?? row.class;
}

describe("policy table", () => {
  it("is spec 1.1", () => {
    expect(table.spec_version).toBe("1.1");
  });

  it("lists each bRequest once, in ascending order", () => {
    const codes = table.commands.map((r) => r.code);
    expect(codes.every((c) => Number.isInteger(c) && c >= 0 && c <= 255)).toBe(true);
    for (let i = 1; i < codes.length; i++) expect(codes[i]).toBeGreaterThan(codes[i - 1]);
  });

  it("gives every row a name, valid directions and a valid class", () => {
    for (const row of table.commands) {
      expect(row.name, `code ${row.code}`).toMatch(/^[A-Z0-9_]+$/);
      expect(row.dirs.length, row.name).toBeGreaterThan(0);
      expect(new Set(row.dirs).size, row.name).toBe(row.dirs.length);
      for (const d of row.dirs) expect(["set", "get"], row.name).toContain(d);
      expect(RANK[row.class], row.name).toBeDefined();
    }
  });

  it("covers exactly the listed directions in `classes`, with `class` as the strictest", () => {
    for (const row of table.commands.filter((r) => r.classes)) {
      expect(Object.keys(row.classes!).sort(), row.name).toEqual([...row.dirs].sort());
      const ranks = row.dirs.map((d) => RANK[classOf(row, d)]);
      expect(ranks.every((r) => r !== undefined), row.name).toBe(true);
      expect(RANK[row.class], row.name).toBe(Math.max(...ranks));
    }
  });

  it("never classes a SET as read, so a viewer can never write", () => {
    for (const row of table.commands.filter((r) => r.dirs.includes("set"))) {
      expect(classOf(row, "set"), row.name).not.toBe("read");
    }
  });

  it("lets a viewer read the limiter but not change it", () => {
    const limiter = table.commands.find((r) => r.code === 0x81)!;
    expect(limiter.name).toBe("LIMITER");
    expect(classOf(limiter, "set")).toBe("control");
    expect(classOf(limiter, "get")).toBe("read");
  });
});
