import { createHash } from "crypto";
import { createMockTransport } from "./createMockTransport";
import { createRelayEngine } from "./createRelayEngine";
import { createReassembler } from "./createReassembler";
import { fragmentMessage } from "./fragmentMessage";

const sha256 = async (bytes: Uint8Array) =>
  createHash("sha256").update(bytes).digest("hex");

function jsonFrames(message: unknown, mtu = 185): Uint8Array[] {
  return fragmentMessage({
    kind: "json",
    stream: 0,
    bytes: new TextEncoder().encode(JSON.stringify(message)),
    mtu,
  });
}

/** Collect JSON messages the engine sends back to the (fake) watch. */
function watchInbox(watch: { onReceive(cb: (bytes: Uint8Array) => void): void }) {
  const messages: any[] = [];
  const reassembler = createReassembler();
  watch.onReceive((bytes) => {
    for (const message of reassembler.feed(bytes)) {
      messages.push(JSON.parse(new TextDecoder().decode(message.bytes)));
    }
  });
  return messages;
}

const flush = () => new Promise((resolve) => setTimeout(resolve, 0));

test("request gets a correlated response with status and body", async () => {
  const { transport, watch } = createMockTransport();
  const inbox = watchInbox(watch);
  const fetchFn = jest.fn(async () =>
    new Response(JSON.stringify([{ item: 41 }]), { status: 200 })
  ) as unknown as typeof fetch;
  createRelayEngine({ transport, fetchFn, sha256 });

  for (const frame of jsonFrames({
    id: 7,
    m: "http",
    p: { method: "GET", url: "https://api.test/outbox" },
  })) {
    watch.send(frame);
  }
  await flush();

  expect(inbox).toEqual([{ id: 7, status: 200, body: [{ item: 41 }] }]);
});

test("event relays without any reply", async () => {
  const { transport, watch } = createMockTransport();
  const inbox = watchInbox(watch);
  const fetchFn = jest.fn(async () => new Response("", { status: 204 })) as unknown as typeof fetch;
  createRelayEngine({ transport, fetchFn, sha256 });

  for (const frame of jsonFrames({
    ev: "http",
    p: { method: "POST", url: "https://api.test/telemetry", body: { steps: 4211 } },
  })) {
    watch.send(frame);
  }
  await flush();

  expect(fetchFn).toHaveBeenCalledTimes(1);
  expect(inbox).toHaveLength(0);
});

test("offline fetch yields an err response, not silence", async () => {
  const { transport, watch } = createMockTransport();
  const inbox = watchInbox(watch);
  const fetchFn = jest.fn(async () => {
    throw new Error("no internet");
  }) as unknown as typeof fetch;
  createRelayEngine({ transport, fetchFn, sha256 });

  for (const frame of jsonFrames({
    id: 3,
    m: "http",
    p: { method: "GET", url: "https://api.test/outbox" },
  })) {
    watch.send(frame);
  }
  await flush();

  expect(inbox).toEqual([{ id: 3, err: { code: "offline", msg: "no internet" } }]);
});

test("streamed upload: staging, acks, sha verify, relay, resume offset", async () => {
  const { transport, watch } = createMockTransport();
  const inbox = watchInbox(watch);
  const posted: Uint8Array[] = [];
  const fetchFn = jest.fn(async (_url: any, init: any) => {
    posted.push(new Uint8Array(init.body));
    return new Response("", { status: 201 });
  }) as unknown as typeof fetch;
  const engine = createRelayEngine({ transport, fetchFn, sha256 });

  const clip = new Uint8Array(2000).map((_, i) => i & 0xff);
  const digest = await sha256(clip);
  const begin = {
    id: 9,
    m: "http",
    p: {
      method: "POST" as const,
      url: "https://api.test/uploads",
      body_stream: { stream: 1, size: clip.length, sha256: digest },
    },
  };
  for (const frame of jsonFrames(begin)) watch.send(frame);
  await flush();
  expect(inbox.shift()).toEqual({ id: 9, cont: { offset: 0 } });

  /* Send 60% of the chunks, then "drop the connection". */
  const chunkSize = 181;
  let sent = 0;
  while (sent < 1200) {
    const chunk = clip.subarray(sent, Math.min(sent + chunkSize, 1200));
    for (const frame of fragmentMessage({ kind: "binary", stream: 1, bytes: chunk, mtu: 185 })) {
      watch.send(frame);
    }
    sent += chunk.length;
  }
  await flush();
  const staged = engine.stagedBytes(digest);
  expect(staged).toBe(1200);

  /* Reconnect: same sha → resume from the staged offset. */
  inbox.length = 0; /* discard interim have-acks from the first session */
  for (const frame of jsonFrames({ ...begin, id: 10 })) watch.send(frame);
  await flush();
  expect(inbox.find((m) => m.cont !== undefined)).toEqual({ id: 10, cont: { offset: 1200 } });

  /* Send the remainder, expect verify + relay + final status. */
  sent = 1200;
  while (sent < clip.length) {
    const chunk = clip.subarray(sent, Math.min(sent + chunkSize, clip.length));
    for (const frame of fragmentMessage({ kind: "binary", stream: 1, bytes: chunk, mtu: 185 })) {
      watch.send(frame);
    }
    sent += chunk.length;
  }
  await flush();
  await flush();

  const final = inbox.filter((m) => m.status !== undefined);
  expect(final).toEqual([{ id: 10, status: 201 }]);
  expect(posted).toHaveLength(1);
  expect(Array.from(posted[0])).toEqual(Array.from(clip));
  expect(engine.stagedBytes(digest)).toBe(0);
});
