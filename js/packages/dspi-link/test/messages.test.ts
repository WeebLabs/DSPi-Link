import { describe, expect, it } from "vitest";
import {
  type HelloHubMessage,
  LinkMessageError,
  decode,
  encode,
  isErr,
  isHubHello,
  isOk,
  pinSourceOf,
} from "../src/messages.js";
import { fixtureJson, fixtureNames } from "./fixtures.js";

const files = fixtureNames("messages", ".json");

describe("control-plane message fixtures", () => {
  it("finds the sample messages", () => {
    expect(files.length).toBeGreaterThan(10);
  });

  for (const file of files) {
    it(`${file} round-trips through decode/encode unchanged`, () => {
      const original = fixtureJson<Record<string, unknown>>("messages", file);
      const decoded = decode(original);
      // Same key set, same values, nothing invented and nothing dropped.
      expect(Object.keys(decoded).sort()).toEqual(Object.keys(original).sort());
      expect(JSON.parse(encode(decoded))).toEqual(original);
      // And again from the text form, which is what arrives on the socket.
      expect(JSON.parse(encode(decode(JSON.stringify(original))))).toEqual(original);
    });
  }
});

describe("message helpers", () => {
  it("tells a hub hello from a client hello", () => {
    const hub = decode(fixtureJson("messages", "hello_hub.json"));
    const client = decode(fixtureJson("messages", "hello_client.json"));
    expect(isHubHello(hub as never)).toBe(true);
    expect(isHubHello(client as never)).toBe(false);
  });

  it("reads pin_source, taking a missing one as window (spec 7.2)", () => {
    const bridge = decode(fixtureJson("messages", "hello_hub_bridge.json")) as HelloHubMessage;
    const console = decode(fixtureJson("messages", "hello_hub.json")) as HelloHubMessage;
    expect(pinSourceOf(bridge)).toBe("fixed");
    expect(pinSourceOf(console)).toBe("window");
  });

  it("recognises ok and err replies", () => {
    expect(isOk(decode(fixtureJson("messages", "auth_ok.json")))).toBe(true);
    expect(isErr(decode(fixtureJson("messages", "err_denied.json")))).toBe(true);
  });

  it("decodes an unknown type to a bare envelope instead of throwing", () => {
    const msg = decode('{"t":"hub.future","id":4,"extra":{"a":1}}');
    expect(msg.t).toBe("hub.future");
    expect(msg.id).toBe(4);
    expect((msg as Record<string, unknown>).extra).toEqual({ a: 1 });
  });

  it("keeps unknown fields on a known type", () => {
    const msg = decode('{"t":"device.list","id":1,"future_field":true}');
    expect((msg as Record<string, unknown>).future_field).toBe(true);
  });

  it("rejects text that is not a JSON object with a string t", () => {
    expect(() => decode("not json")).toThrow(LinkMessageError);
    expect(() => decode("[1,2]")).toThrow(LinkMessageError);
    expect(() => decode('{"id":1}')).toThrow(LinkMessageError);
    expect(() => decode("null")).toThrow(LinkMessageError);
  });
});
