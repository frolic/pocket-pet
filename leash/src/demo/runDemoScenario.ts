import { createMockTransport } from "../protocol/createMockTransport";
import { createRelayEngine } from "../protocol/createRelayEngine";
import { createReassembler } from "../protocol/createReassembler";
import { fragmentMessage } from "../protocol/fragmentMessage";

/* A scripted fake watch + fake server driving the real relay engine —
   the whole pipeline minus BLE, runnable in the simulator. The scenario
   mirrors the design doc's sequence diagrams: telemetry event, outbox
   drain, then a chunked upload that "drops" mid-transfer and resumes. */

export function runDemoScenario(options: {
  sha256: (bytes: Uint8Array) => Promise<string>;
  log: (line: string) => void;
}) {
  const { sha256, log } = options;
  const { transport, watch } = createMockTransport();

  /* Fake server the "phone" relays to. */
  const demoFetch = (async (url: RequestInfo | URL, init?: RequestInit) => {
    const path = String(url);
    await new Promise((resolve) => setTimeout(resolve, 120));
    if (path.endsWith("/telemetry")) return new Response("", { status: 204 });
    if (path.includes("/outbox")) {
      return new Response(JSON.stringify([{ kind: "msg", body: "feed me" }]), { status: 200 });
    }
    if (path.endsWith("/uploads")) {
      const size = init?.body instanceof Uint8Array ? init.body.length : 0;
      log(`  server: received upload of ${size} bytes`);
      return new Response("", { status: 201 });
    }
    return new Response("not found", { status: 404 });
  }) as typeof fetch;

  createRelayEngine({ transport, fetchFn: demoFetch, sha256, log: (l) => log(`engine: ${l}`) });

  /* Watch-side plumbing: send JSON, collect responses. */
  const inbox: any[] = [];
  const reassembler = createReassembler();
  watch.onReceive((bytes) => {
    for (const message of reassembler.feed(bytes)) {
      const parsed = JSON.parse(new TextDecoder().decode(message.bytes));
      inbox.push(parsed);
      log(`watch ← ${JSON.stringify(parsed)}`);
    }
  });
  const sendJson = (message: unknown) => {
    log(`watch → ${JSON.stringify(message)}`);
    for (const frame of fragmentMessage({
      kind: "json",
      stream: 0,
      bytes: new TextEncoder().encode(JSON.stringify(message)),
      mtu: 185,
    })) {
      watch.send(frame);
    }
  };
  const waitFor = async (predicate: (m: any) => boolean) => {
    for (let i = 0; i < 100; i++) {
      const found = inbox.find(predicate);
      if (found) return found;
      await new Promise((resolve) => setTimeout(resolve, 30));
    }
    throw new Error("demo: timed out waiting for a response");
  };

  async function run() {
    log("— telemetry (fire-and-forget) —");
    sendJson({ ev: "http", p: { method: "POST", url: "https://demo/telemetry", body: { steps: 4211, bat: 87 } } });

    log("— outbox drain —");
    sendJson({ id: 1, m: "http", p: { method: "GET", url: "https://demo/outbox?ack=" } });
    await waitFor((m) => m.id === 1 && m.status === 200);

    log("— upload with mid-transfer drop —");
    const clip = new Uint8Array(4000).map((_, i) => (i * 31) & 0xff);
    const digest = await sha256(clip);
    const begin = (id: number) => ({
      id,
      m: "http",
      p: {
        method: "POST" as const,
        url: "https://demo/uploads",
        body_stream: { stream: 1, size: clip.length, sha256: digest },
      },
    });
    sendJson(begin(2));
    await waitFor((m) => m.id === 2 && m.cont !== undefined);
    let sent = 0;
    while (sent < 2400) {
      const chunk = clip.subarray(sent, Math.min(sent + 181, 2400));
      for (const frame of fragmentMessage({ kind: "binary", stream: 1, bytes: chunk, mtu: 185 })) {
        watch.send(frame);
      }
      sent += chunk.length;
    }
    log("✗ connection dropped at 60% … reconnecting");
    await new Promise((resolve) => setTimeout(resolve, 400));

    sendJson(begin(3));
    const cont = await waitFor((m) => m.id === 3 && m.cont !== undefined);
    log(`resumed at offset ${cont.cont.offset} (no bytes resent)`);
    sent = cont.cont.offset;
    while (sent < clip.length) {
      const chunk = clip.subarray(sent, Math.min(sent + 181, clip.length));
      for (const frame of fragmentMessage({ kind: "binary", stream: 1, bytes: chunk, mtu: 185 })) {
        watch.send(frame);
      }
      sent += chunk.length;
    }
    await waitFor((m) => m.id === 3 && m.status === 201);
    log("✓ upload verified (sha256) and relayed — demo complete");
  }

  return run();
}
