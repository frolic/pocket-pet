import { decodeFrame } from "./decodeFrame";
import { encodeFrame } from "./encodeFrame";
import { fragmentMessage } from "./fragmentMessage";
import { createReassembler } from "./createReassembler";

const MTU = 185;

function bytesOf(length: number, seed = 7): Uint8Array {
  const bytes = new Uint8Array(length);
  for (let i = 0; i < length; i++) bytes[i] = (i * seed) & 0xff;
  return bytes;
}

test("frame round-trips header and payload", () => {
  const frame = {
    kind: "binary" as const,
    position: "last" as const,
    stream: 1,
    payload: bytesOf(40),
  };
  const decoded = decodeFrame(encodeFrame(frame));
  expect(decoded.kind).toBe("binary");
  expect(decoded.position).toBe("last");
  expect(decoded.stream).toBe(1);
  expect(Array.from(decoded.payload)).toEqual(Array.from(frame.payload));
});

test.each([1, MTU - 5, MTU - 4, MTU - 3, MTU * 3, 1000])(
  "fragment + reassemble round-trips %d bytes",
  (size) => {
    const original = bytesOf(size);
    const frames = fragmentMessage({ kind: "json", stream: 0, bytes: original, mtu: MTU });
    for (const frame of frames) expect(frame.length).toBeLessThanOrEqual(MTU);
    const reassembler = createReassembler();
    const messages = frames.flatMap((frame) => reassembler.feed(frame));
    expect(messages).toHaveLength(1);
    expect(Array.from(messages[0].bytes)).toEqual(Array.from(original));
  }
);

test("streams interleave without corrupting each other", () => {
  const control = bytesOf(400, 3);
  const bulk = bytesOf(500, 11);
  const controlFrames = fragmentMessage({ kind: "json", stream: 0, bytes: control, mtu: MTU });
  const bulkFrames = fragmentMessage({ kind: "binary", stream: 1, bytes: bulk, mtu: MTU });
  const reassembler = createReassembler();
  const messages = [];
  const lanes = [controlFrames, bulkFrames];
  const cursors = [0, 0];
  let turn = 0;
  while (cursors[0] < lanes[0].length || cursors[1] < lanes[1].length) {
    const lane = cursors[turn % 2] < lanes[turn % 2].length ? turn % 2 : (turn + 1) % 2;
    messages.push(...reassembler.feed(lanes[lane][cursors[lane]]));
    cursors[lane]++;
    turn++;
  }
  expect(messages).toHaveLength(2);
  const byStream = new Map(messages.map((m) => [m.stream, m]));
  expect(Array.from(byStream.get(0)!.bytes)).toEqual(Array.from(control));
  expect(Array.from(byStream.get(1)!.bytes)).toEqual(Array.from(bulk));
});

test("a fresh 'first' frame abandons a stale partial on the same stream", () => {
  const reassembler = createReassembler();
  const stale = fragmentMessage({ kind: "json", stream: 0, bytes: bytesOf(600), mtu: MTU });
  reassembler.feed(stale[0]); /* first arrives, rest lost in a disconnect */
  const fresh = fragmentMessage({ kind: "json", stream: 0, bytes: bytesOf(300, 5), mtu: MTU });
  const messages = fresh.flatMap((frame) => reassembler.feed(frame));
  expect(messages).toHaveLength(1);
  expect(messages[0].bytes.length).toBe(300);
});
