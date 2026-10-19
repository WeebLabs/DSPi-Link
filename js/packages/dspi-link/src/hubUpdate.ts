/*
 * The hub firmware upload engine (Link spec 7.14 and 8.5).
 *
 * Sends an image as FWDATA chunks, a few ahead of their acks, and waits for
 * the hub's verdict in hub.update.done. It is apart from LinkClient because
 * it is the one long-running, windowed transfer in the protocol; the client
 * owns one HubUpdater and hands it the acks it receives. It reaches the
 * session only through the HubUpdateTransport it is given.
 */

import { asError, LinkProtocolError } from "./errors.js";
import { type FwDataAckFrame, type FwDataFrame, FWDATA_HEADER_BYTES } from "./frames.js";
import type { HubUpdateDoneMessage, HubUpdateMessage, LinkMessage, OkMessage } from "./messages.js";
import { isMessage } from "./messages.js";
import { sha256Hex } from "./sha256.js";

/** What updateHub takes besides the image. */
export interface HubUpdateOptions {
  /** The new firmware's version, for the hub's log; optional (Link spec 7.14). */
  version?: string;
  /** Called as chunks are acknowledged, with the bytes written so far. */
  onProgress?: (written: number, total: number) => void;
}

/** The parts of a session the upload needs. LinkClient provides them. */
export interface HubUpdateTransport {
  /** Sends a control request and resolves with its ok, or rejects. */
  request(message: LinkMessage, timeoutMs?: number): Promise<OkMessage>;
  /** Calls `listener` for every control message; returns the unsubscribe function. */
  onMessage(listener: (message: LinkMessage) => void): () => void;
  /** Sends one FWDATA frame; throws when the session is closed. */
  sendFrame(frame: FwDataFrame): void;
  /** The hub's max_frame, which bounds a chunk (Link spec 8.5). */
  maxFrameBytes(): number;
}

/** FWDATA chunks sent ahead of their acks. A bridge holds two waiting to be
 * written (bridge spec 14.2), so a third would be refused; Link spec 8.5 says
 * two is always safe. */
const UPDATE_WINDOW_CHUNKS = 2;

/** The longest a chunk may wait for its ack. Writing to flash can pause
 * for a few seconds; the hub gives up on a stalled update after 30 s. */
const UPDATE_ACK_TIMEOUT_MS = 35000;

/** How long hub.update.done may take after the last ack: the hub checks
 * the whole image first. */
const UPDATE_DONE_TIMEOUT_MS = 30000;

/** After a chunk fails, how long to wait for a hub.update.done that explains
 * why, which usually follows right behind. */
const UPDATE_REFUSAL_GRACE_MS = 2000;

interface PendingAck {
  resolve: () => void;
  reject: (error: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

interface NextMessage<T> {
  promise: Promise<T>;
  cancel: () => void;
}

/** Runs hub firmware updates over one session. Owned by LinkClient. */
export class HubUpdater {
  /** Chunks waiting for their acks, keyed "transfer:offset". */
  private readonly pendingAcks = new Map<string, PendingAck>();

  /** @param transport The session to upload over. */
  constructor(private readonly transport: HubUpdateTransport) {}

  /**
   * Uploads `image` and waits for the hub to accept it.
   *
   * @param image The whole firmware file.
   * @param options The version to log and a progress callback.
   * @returns The version the hub reports it will restart into.
   * @throws LinkProtocolError with the hub's reason when it refuses the
   *   request or the image; Error on a timeout or a lost connection.
   */
  async run(image: Uint8Array, options: HubUpdateOptions = {}): Promise<string> {
    const request: HubUpdateMessage = {
      t: "hub.update",
      id: 0,
      size: image.length,
      sha256: sha256Hex(image),
    };
    if (options.version) request.version = options.version;

    // Listen before asking, since the verdict can follow the ok closely.
    const done = this.nextUpdateDone();
    let transfer: number | null = null;
    try {
      transfer = transferOf(await this.transport.request(request));
      await Promise.race([
        this.sendImage(transfer, image, options.onProgress),
        refusalOf(done.promise),
      ]);
      const verdict = await withTimeout(done.promise, UPDATE_DONE_TIMEOUT_MS, "hub.update.done");
      if (!verdict.ok) throw refusedError(verdict);
      return verdict.version ?? options.version ?? "";
    } catch (error) {
      // A refused request has no transfer and no verdict to wait for.
      if (transfer === null) throw error;
      const verdict = await withTimeout(done.promise, UPDATE_REFUSAL_GRACE_MS, "").catch(
        () => null,
      );
      if (verdict && !verdict.ok) throw refusedError(verdict);
      throw error;
    } finally {
      done.cancel();
      if (transfer !== null) this.dropAcks(transfer);
    }
  }

  /**
   * Settles the chunk an ack answers. Acks for unknown chunks are ignored.
   *
   * @param frame An FWDATA ack from the hub.
   */
  settleAck(frame: FwDataAckFrame): void {
    const key = ackKey(frame.tag, frame.offset);
    const pending = this.pendingAcks.get(key);
    if (!pending) return;
    this.pendingAcks.delete(key);
    clearTimeout(pending.timer);
    if (frame.status === 0) pending.resolve();
    else pending.reject(new Error(`the hub refused the chunk at offset ${frame.offset}`));
  }

  /**
   * Rejects every chunk still waiting, when the session closes.
   *
   * @param error The reason to reject with.
   */
  failAll(error: Error): void {
    for (const pending of this.pendingAcks.values()) {
      clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.pendingAcks.clear();
  }

  /** Forgets the chunks of `transfer` still waiting, once the update is over either way. */
  private dropAcks(transfer: number): void {
    for (const [key, pending] of this.pendingAcks) {
      if (key.startsWith(`${transfer}:`)) {
        clearTimeout(pending.timer);
        this.pendingAcks.delete(key);
      }
    }
  }

  /** Sends the chunks of one transfer, keeping UPDATE_WINDOW_CHUNKS unanswered. */
  private async sendImage(
    transfer: number,
    image: Uint8Array,
    onProgress?: (written: number, total: number) => void,
  ): Promise<void> {
    const chunkBytes = this.transport.maxFrameBytes() - FWDATA_HEADER_BYTES;
    const inFlight: Array<Promise<number>> = [];
    let written = 0;
    for (let offset = 0; offset < image.length; offset += chunkBytes) {
      const oldest = inFlight.length >= UPDATE_WINDOW_CHUNKS ? inFlight.shift() : undefined;
      if (oldest) {
        written += await oldest;
        onProgress?.(written, image.length);
      }
      const data = image.subarray(offset, offset + chunkBytes);
      const chunk = this.sendChunk(transfer, offset, data).then(() => data.length);
      // Awaited in turn below; this only stops a later chunk failing first
      // from being reported as an unhandled rejection.
      chunk.catch(() => undefined);
      inFlight.push(chunk);
    }
    for (const chunk of inFlight) {
      written += await chunk;
      onProgress?.(written, image.length);
    }
  }

  /** Sends one chunk and resolves when the hub acknowledges it. */
  private sendChunk(transfer: number, offset: number, data: Uint8Array): Promise<void> {
    const key = ackKey(transfer, offset);
    return new Promise<void>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pendingAcks.delete(key);
        reject(new Error(`no answer for the chunk at offset ${offset}`));
      }, UPDATE_ACK_TIMEOUT_MS);
      this.pendingAcks.set(key, { resolve, reject, timer });
      try {
        this.transport.sendFrame({ type: "fwdata", tag: transfer, offset, data });
      } catch (error) {
        clearTimeout(timer);
        this.pendingAcks.delete(key);
        reject(asError(error));
      }
    });
  }

  /** The next hub.update.done, as a promise that can be called off. */
  private nextUpdateDone(): NextMessage<HubUpdateDoneMessage> {
    let stop = (): void => undefined;
    const promise = new Promise<HubUpdateDoneMessage>((resolve) => {
      stop = this.transport.onMessage((message) => {
        if (!isMessage(message, "hub.update.done")) return;
        stop();
        resolve(message);
      });
    });
    return { promise, cancel: () => stop() };
  }
}

function ackKey(transfer: number, offset: number): string {
  return `${transfer}:${offset}`;
}

/** The `xfer` of the ok to hub.update, which every chunk carries as its tag. */
function transferOf(reply: OkMessage): number {
  const transfer = reply.xfer;
  if (typeof transfer !== "number" || !Number.isInteger(transfer)) {
    throw new LinkProtocolError("internal", "the hub's reply to hub.update has no transfer id");
  }
  return transfer;
}

function refusedError(verdict: HubUpdateDoneMessage): LinkProtocolError {
  return new LinkProtocolError("internal", verdict.msg ?? "update refused");
}

/**
 * A promise that rejects if the verdict is a refusal and otherwise never
 * settles, so a refusal can end the upload early with the hub's own reason
 * rather than a bare "chunk refused".
 */
function refusalOf(verdict: Promise<HubUpdateDoneMessage>): Promise<never> {
  const refusal = verdict.then((message) => {
    if (!message.ok) throw refusedError(message);
    return new Promise<never>(() => undefined);
  });
  // Raced by the caller; this only keeps a refusal after the race from being unhandled.
  refusal.catch(() => undefined);
  return refusal;
}

/** `promise`, or a rejection after `timeoutMs` naming `what`. */
function withTimeout<T>(promise: Promise<T>, timeoutMs: number, what: string): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${what} timed out`)), timeoutMs);
    promise.then(
      (value) => {
        clearTimeout(timer);
        resolve(value);
      },
      (error: unknown) => {
        clearTimeout(timer);
        reject(asError(error));
      },
    );
  });
}
