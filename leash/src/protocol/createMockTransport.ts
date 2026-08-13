import { Transport } from "./common";

/** In-memory transport pair: what the engine sees, and the handle a fake
    watch (tests, simulator demo) uses to play the other side. */
export function createMockTransport(options?: { mtu?: number }) {
  const mtu = options?.mtu ?? 185;
  let engineReceive: ((bytes: Uint8Array) => void) | undefined;
  const sentToWatch: Uint8Array[] = [];
  let watchReceive: ((bytes: Uint8Array) => void) | undefined;

  const transport: Transport = {
    mtu: () => mtu,
    send: async (bytes) => {
      sentToWatch.push(bytes);
      watchReceive?.(bytes);
    },
    onReceive: (callback) => {
      engineReceive = callback;
    },
  };

  return {
    transport,
    watch: {
      /** Deliver frames from the fake watch into the engine. */
      send(bytes: Uint8Array): void {
        engineReceive?.(bytes);
      },
      onReceive(callback: (bytes: Uint8Array) => void): void {
        watchReceive = callback;
      },
      sent: sentToWatch,
    },
  };
}
