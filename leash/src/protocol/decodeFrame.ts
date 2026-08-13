import { Frame, FrameKind, FramePosition } from "./common";

const KINDS: FrameKind[] = ["json", "binary"];
const POSITIONS: FramePosition[] = ["only", "first", "cont", "last"];

/** Inverse of encodeFrame; throws on a malformed header or length. */
export function decodeFrame(bytes: Uint8Array): Frame {
  if (bytes.length < 4) throw new Error(`frame too short: ${bytes.length}`);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const length = view.getUint16(0, true);
  if (bytes.length !== 4 + length) {
    throw new Error(`frame length mismatch: header ${length}, actual ${bytes.length - 4}`);
  }
  const kind = KINDS[bytes[2] & 0b11];
  const position = POSITIONS[(bytes[2] >> 2) & 0b11];
  if (kind === undefined) throw new Error(`unknown frame kind: ${bytes[2] & 0b11}`);
  return { kind, position, stream: bytes[3], payload: bytes.subarray(4) };
}
