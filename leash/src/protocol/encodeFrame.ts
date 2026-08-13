import { Frame, FrameKind, FramePosition } from "./common";

const KIND_BITS: Record<FrameKind, number> = { json: 0b00, binary: 0b01 };
const POSITION_BITS: Record<FramePosition, number> = {
  only: 0b00,
  first: 0b01,
  cont: 0b10,
  last: 0b11,
};

/** Wire layout: [len:u16 LE] [flags:u8] [stream:u8] [payload]. */
export function encodeFrame(frame: Frame): Uint8Array {
  const bytes = new Uint8Array(4 + frame.payload.length);
  const view = new DataView(bytes.buffer);
  view.setUint16(0, frame.payload.length, true);
  bytes[2] = KIND_BITS[frame.kind] | (POSITION_BITS[frame.position] << 2);
  bytes[3] = frame.stream;
  bytes.set(frame.payload, 4);
  return bytes;
}
