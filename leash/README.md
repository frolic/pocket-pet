# leash

Phone-side relay for the pocket-pet watch: the watch makes HTTP calls,
BLE is the wire, this app is the modem. Design:
[`../docs/ble-gateway-design.md`](../docs/ble-gateway-design.md).

- `src/protocol/` — transport-agnostic core (framing, reassembly, relay
  engine with sha-resumable streamed bodies). `npm test`.
- `src/demo/` — scripted mock watch driving the real engine; `npm run
  ios` shows the full pipeline in the simulator (no Bluetooth needed).
- BLE transport lands at P2 behind the `Transport` seam, once the watch
  firmware speaks the protocol (P1). Device builds need
  `npx expo run:ios --device` (dev build; ble-plx is a native module —
  Expo Go won't carry it).
