/*
 * The errors LinkClient rejects with, so a caller can tell a hub's refusal
 * from a lost connection.
 *
 * A control-plane `err` reply (Link spec 7.1) becomes a LinkProtocolError
 * carrying the hub's code; a CMD response whose status is not OK (Link spec
 * 8.1) becomes a LinkCommandError for callers that prefer a throw. Anything
 * else (a timeout, a closed socket) is a plain Error.
 */

import { type CmdResponseFrame, statusName } from "./frames.js";
import type { LinkErrorCode } from "./messages.js";

/** A control-plane `err` reply turned into a rejection. */
export class LinkProtocolError extends Error {
  /** The hub's error code, such as "denied" or "busy". */
  readonly code: LinkErrorCode;
  /** The hub's own sentence, when it gave one. */
  readonly detail?: string;
  /** For a rejected hub.config.set: the offending key, as "uart.tx_pin". */
  readonly field?: string;

  /**
   * @param code The `code` of the err reply.
   * @param message The reply's `msg`, if any.
   * @param field The reply's `field`, if any.
   */
  constructor(code: LinkErrorCode, message?: string, field?: string) {
    super(message ? `${code}: ${message}` : code);
    this.name = "LinkProtocolError";
    this.code = code;
    this.detail = message;
    this.field = field;
  }
}

/** A CMD response whose status was not OK, for callers that want a throw. */
export class LinkCommandError extends Error {
  /** The response's status byte (Link spec 8.1). */
  readonly status: number;
  /** The whole response, payload included. */
  readonly response: CmdResponseFrame;

  /** @param response The CMD response that carried a non-OK status. */
  constructor(response: CmdResponseFrame) {
    super(`command failed: ${statusName(response.status)}`);
    this.name = "LinkCommandError";
    this.status = response.status;
    this.response = response;
  }
}

/** `value` as an Error, wrapping anything else that was thrown. */
export function asError(value: unknown): Error {
  return value instanceof Error ? value : new Error(String(value));
}
