import { WireMessage } from "./common";
import { decodeFrame } from "./decodeFrame";

/** Per-stream reassembly: feed raw frames, get completed messages. A
    "first" frame implicitly abandons any half-built message on the same
    stream (the peer restarted after a drop — resume is app-level). */
export function createReassembler() {
  const partial = new Map<number, { kind: string; parts: Uint8Array[] }>();

  function feed(raw: Uint8Array): WireMessage[] {
    const frame = decodeFrame(raw);
    const key = frame.stream;
    if (frame.position === "only") {
      partial.delete(key);
      return [{ kind: frame.kind, stream: frame.stream, bytes: frame.payload }];
    }
    if (frame.position === "first") {
      partial.set(key, { kind: frame.kind, parts: [frame.payload] });
      return [];
    }
    const pending = partial.get(key);
    if (pending === undefined || pending.kind !== frame.kind) {
      partial.delete(key); /* orphan continuation: drop, resume is app-level */
      return [];
    }
    pending.parts.push(frame.payload);
    if (frame.position === "cont") return [];
    partial.delete(key);
    const total = pending.parts.reduce((sum, part) => sum + part.length, 0);
    const bytes = new Uint8Array(total);
    let offset = 0;
    for (const part of pending.parts) {
      bytes.set(part, offset);
      offset += part.length;
    }
    return [{ kind: frame.kind as WireMessage["kind"], stream: frame.stream, bytes }];
  }

  return { feed };
}
