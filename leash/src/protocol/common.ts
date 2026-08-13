/* Shared types for the Leash protocol (see docs/ble-gateway-design.md in
   the pocket-pikachu repo). One invariant: all intent originates on the
   device — the phone only ever answers. */

export type FrameKind = "json" | "binary";

export type FramePosition = "only" | "first" | "cont" | "last";

export interface Frame {
  kind: FrameKind;
  position: FramePosition;
  stream: number;
  payload: Uint8Array;
}

/** A completed (reassembled) message from the wire. */
export interface WireMessage {
  kind: FrameKind;
  stream: number;
  bytes: Uint8Array;
}

/** The one seam between protocol logic and BLE (or a mock). */
export interface Transport {
  /** Max bytes per send, including the 4-byte frame header. */
  mtu(): number;
  send(bytes: Uint8Array): Promise<void>;
  onReceive(callback: (bytes: Uint8Array) => void): void;
}

/** Device request/event payload for the single verb: an HTTP call. */
export interface HttpParams {
  method: "GET" | "POST" | "PUT" | "DELETE";
  url: string;
  headers?: Record<string, string>;
  body?: unknown;
  body_stream?: { stream: number; size: number; sha256: string };
}

export interface HttpResult {
  status: number;
  body?: unknown;
}

export interface RelayError {
  code: "offline" | "http_error" | "bad_request" | "sha_mismatch";
  msg: string;
}
