/*
 * Turns what a Link request rejected with into a sentence for the screen
 * (bridge spec 10.4: plain language). The client's own messages ("request
 * auth.pair timed out", "connection closed (1006)") are for logs, not people.
 */

import { LinkProtocolError } from "@dspi/link";

/**
 * A sentence saying what went wrong.
 *
 * @param error What the request rejected with.
 * @param subject What the user was talking to: "hub", "bridge" or "DSPi".
 * @returns The sentence.
 */
export function describeProblem(error: unknown, subject = "hub"): string {
  if (error instanceof LinkProtocolError) {
    switch (error.code) {
      case "denied":
        return `This browser isn't allowed to do that on this ${subject}.`;
      case "busy":
      case "locked":
        return `The ${subject} is busy with something else. Try again in a moment.`;
      case "rate_limited":
        return "Too many tries. Wait a minute and try again.";
      case "no_device":
        return "That DSPi isn't connected any more.";
    }
    const reason = error.detail?.replace(/\.$/, "");
    return reason ? `The ${subject} refused: ${reason}.` : `The ${subject} refused the request.`;
  }
  const message = error instanceof Error ? error.message : String(error);
  if (/timed out|did not answer/.test(message)) {
    return `The ${subject} didn't answer in time. Try again.`;
  }
  if (/during handshake/.test(message)) {
    return `Couldn't connect to the ${subject}. Check the address and that it is switched on.`;
  }
  if (/closed|not connected|disconnected/.test(message)) {
    return `The connection to the ${subject} dropped.`;
  }
  if (/^hub info:/.test(message)) {
    return "Something answered at that address, but it isn't a DSPi hub.";
  }
  if (/fetch|network|abort/i.test(message)) {
    return `Couldn't reach the ${subject}. Check the address and that this device is on the same network.`;
  }
  return `Something went wrong: ${message}.`;
}
