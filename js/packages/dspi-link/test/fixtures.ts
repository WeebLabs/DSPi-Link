import { readFileSync, readdirSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

// The fixtures live at the repository root, two levels above js/packages/.
const here = dirname(fileURLToPath(import.meta.url));
export const FIXTURES = join(here, "..", "..", "..", "..", "fixtures");

export function fixtureBytes(...parts: string[]): Uint8Array {
  return new Uint8Array(readFileSync(join(FIXTURES, ...parts)));
}

export function fixtureJson<T = unknown>(...parts: string[]): T {
  return JSON.parse(readFileSync(join(FIXTURES, ...parts), "utf8")) as T;
}

export function fixtureNames(dir: string, ext: string): string[] {
  return readdirSync(join(FIXTURES, dir))
    .filter((n) => n.endsWith(ext))
    .sort();
}

export function toHex(bytes: Uint8Array): string {
  return [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
}
