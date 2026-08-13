import { HttpParams, Transport, WireMessage } from "./common";
import { createReassembler } from "./createReassembler";
import { executeHttpRequest } from "./executeHttpRequest";
import { fragmentMessage } from "./fragmentMessage";

const ACK_EVERY_CHUNKS = 8;

interface DeviceMessage {
  ev?: string;
  id?: number;
  m?: string;
  p?: HttpParams;
}

interface Staging {
  sha256: string;
  size: number;
  received: number;
  parts: Uint8Array[];
  requestId: number;
  params: HttpParams;
  chunksSinceAck: number;
  /** Completion may only run once: a chunk handler suspended on an ack
      await can resume after a later chunk already finished the upload. */
  finishing: boolean;
}

/** The whole phone app, minus I/O: answers device HTTP requests, relays
    fire-and-forget events, stages streamed upload bodies with sha-based
    resume. Stateless per request except the staging area, which is keyed
    by sha256 precisely so it survives reconnects. */
export function createRelayEngine(options: {
  transport: Transport;
  fetchFn: typeof fetch;
  sha256: (bytes: Uint8Array) => Promise<string>;
  log?: (line: string) => void;
}) {
  const { transport, fetchFn, sha256 } = options;
  const log = options.log ?? (() => {});
  const reassembler = createReassembler();
  /* Keyed by sha256 so an interrupted upload resumes across reconnects. */
  const stagingBySha = new Map<string, Staging>();
  let activeStream: Staging | undefined;

  async function sendJson(message: unknown): Promise<void> {
    const bytes = new TextEncoder().encode(JSON.stringify(message));
    for (const frame of fragmentMessage({
      kind: "json",
      stream: 0,
      bytes,
      mtu: transport.mtu(),
    })) {
      await transport.send(frame);
    }
  }

  async function runHttp(id: number | undefined, params: HttpParams, bodyBytes?: Uint8Array) {
    const outcome = await executeHttpRequest({ params, bodyBytes, fetchFn });
    if (id === undefined) return; /* event: fire-and-forget */
    if ("error" in outcome) {
      await sendJson({ id, err: outcome.error });
    } else {
      await sendJson({ id, status: outcome.status, body: outcome.body });
    }
  }

  async function handleJson(message: DeviceMessage): Promise<void> {
    if (message.ev === "http" && message.p !== undefined) {
      log(`event http ${message.p.method} ${message.p.url}`);
      await runHttp(undefined, message.p);
      return;
    }
    if (message.id === undefined || message.m !== "http" || message.p === undefined) {
      log(`dropped malformed message: ${JSON.stringify(message)}`);
      return;
    }
    const params = message.p;
    if (params.body_stream === undefined) {
      log(`request ${message.id}: ${params.method} ${params.url}`);
      await runHttp(message.id, params);
      return;
    }
    /* Streamed body: open (or resume) staging, invite chunks. */
    const existing = stagingBySha.get(params.body_stream.sha256);
    const staging: Staging = existing ?? {
      sha256: params.body_stream.sha256,
      size: params.body_stream.size,
      received: 0,
      parts: [],
      requestId: message.id,
      params,
      chunksSinceAck: 0,
      finishing: false,
    };
    staging.requestId = message.id;
    staging.params = params;
    stagingBySha.set(staging.sha256, staging);
    activeStream = staging;
    log(`upload ${message.id}: resume at ${staging.received}/${staging.size}`);
    await sendJson({ id: message.id, cont: { offset: staging.received } });
  }

  async function handleBinary(message: WireMessage): Promise<void> {
    const staging = activeStream;
    if (staging === undefined) {
      log("dropped orphan binary chunk");
      return;
    }
    staging.parts.push(message.bytes);
    staging.received += message.bytes.length;
    staging.chunksSinceAck += 1;
    if (staging.chunksSinceAck >= ACK_EVERY_CHUNKS && staging.received < staging.size) {
      staging.chunksSinceAck = 0;
      await sendJson({ id: staging.requestId, have: staging.received });
    }
    if (staging.received < staging.size) return;
    if (staging.finishing) return;
    staging.finishing = true;

    /* Body complete: verify, then run the HTTP call it belongs to. */
    const bytes = new Uint8Array(staging.size);
    let offset = 0;
    for (const part of staging.parts) {
      bytes.set(part, offset);
      offset += part.length;
    }
    const digest = await sha256(bytes);
    if (digest !== staging.sha256) {
      log(`upload ${staging.requestId}: sha mismatch`);
      stagingBySha.delete(staging.sha256);
      activeStream = undefined;
      await sendJson({
        id: staging.requestId,
        err: { code: "sha_mismatch", msg: `expected ${staging.sha256}, got ${digest}` },
      });
      return;
    }
    log(`upload ${staging.requestId}: complete, relaying ${staging.size} bytes`);
    await runHttp(staging.requestId, staging.params, bytes);
    stagingBySha.delete(staging.sha256);
    activeStream = undefined;
  }

  transport.onReceive((raw) => {
    for (const message of reassembler.feed(raw)) {
      const handler =
        message.kind === "json"
          ? handleJson(JSON.parse(new TextDecoder().decode(message.bytes)))
          : handleBinary(message);
      handler.catch((error) => log(`relay error: ${error}`));
    }
  });

  return {
    /** Bytes staged for an interrupted upload (test/UI introspection). */
    stagedBytes(sha: string): number {
      return stagingBySha.get(sha)?.received ?? 0;
    },
  };
}
