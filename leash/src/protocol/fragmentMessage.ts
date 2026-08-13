import { Frame, FrameKind } from "./common";
import { encodeFrame } from "./encodeFrame";

/** Splits one message into MTU-sized frames, ready to send in order. */
export function fragmentMessage(options: {
  kind: FrameKind;
  stream: number;
  bytes: Uint8Array;
  mtu: number;
}): Uint8Array[] {
  const { kind, stream, bytes, mtu } = options;
  const chunk = mtu - 4;
  if (chunk < 1) throw new Error(`mtu ${mtu} leaves no payload room`);
  if (bytes.length <= chunk) {
    return [encodeFrame({ kind, position: "only", stream, payload: bytes })];
  }
  const frames: Uint8Array[] = [];
  for (let offset = 0; offset < bytes.length; offset += chunk) {
    const payload = bytes.subarray(offset, Math.min(offset + chunk, bytes.length));
    const position =
      offset === 0 ? "first" : offset + chunk >= bytes.length ? "last" : "cont";
    frames.push(encodeFrame({ kind, position, stream, payload } as Frame));
  }
  return frames;
}
