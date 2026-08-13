# Leash — device-initiated BLE gateway

*Design doc, 2026-08-13. Status: proposed (no firmware or app code exists yet).*

The watch talks to the internet through the phone over BLE, replacing wifi
as the daily transport. One invariant governs everything:

> **All intent originates on the device.**
> The watch pushes when it wants, asks when it needs. The phone only
> relays; the server only queues. Nothing upstream can demand the watch's
> attention — which matches the radio physics: a sleeping watch has no
> radio to demand attention from.

## The pieces

```mermaid
flowchart LR
    subgraph watch["watch (ESP32-S3)"]
        APP[pet app] --> LSH[leash client]
        LSH --> NIM[NimBLE peripheral]
    end
    subgraph phone["iPhone (Expo app)"]
        BLE[ble-plx central] --> ENG[relay engine]
        ENG --> KC[(keychain: tokens)]
    end
    subgraph cloud["your API"]
        IN[ingest endpoints]
        OUT[(outbox queue)]
    end
    NIM <-- "GATT: notify / write" --> BLE
    ENG -- "POST telemetry, files" --> IN
    ENG -- "GET inbox" --> OUT
```

The phone app holds **no product logic**. It wakes when the watch speaks,
does what the watch asks (using routes from the watch's own manifest),
and goes back to sleep. New watch behavior never requires an app update.

## Who may say what

```mermaid
flowchart TD
    W[watch] -- "1 · events (fire-and-forget)" --> P[phone]
    W -- "2 · requests {id, method}" --> P
    P -- "3 · responses {id, result} ONLY" --> W
    P -- "webhooks / drains" --> S[server]
    S -- "answers + queued outbox items" --> P
    style W stroke-width:3px
```

A phone→watch write that carries no known request `id` is a protocol
error and is dropped. There is no path for the server or phone to
initiate anything — deleting APNs push infrastructure, the device-side
pending-request table, and every unsolicited-message edge case.

## GATT layout

One service, three characteristics. Everything conversational shares one
duplex framed channel; the manifest is a plain read.

| Characteristic | Props | Carries |
|---|---|---|
| `MANIFEST` | read | Device-authored JSON: identity, routes, protocol version |
| `TX` | notify | Watch → phone frames (events, requests, file chunks) |
| `RX` | write | Phone → watch frames (responses, download chunks) |

The watch controls the session at every layer: **advertising is the
device saying "open for business."** Radio dark = nothing exists to
connect to.

## Session lifecycle (watch side)

Mirrors the existing radio-follows-screen policy — BLE simply replaces
wifi in the same slot.

```mermaid
stateDiagram-v2
    [*] --> RadioOff
    RadioOff: RadioOff (dark / light-sleeping)
    Advertising: Advertising (screen on, ~1s interval)
    Connected: Connected (phone found us)
    Draining: Ask cadence (on connect + every N min)
    RadioOff --> Advertising: screen on
    Advertising --> Connected: phone connects
    Connected --> Draining: connect / timer / app intent
    Draining --> Connected: idle
    Connected --> RadioOff: screen dark
    Advertising --> RadioOff: screen dark
    Connected --> Advertising: connection dropped
```

`sleep_eligible` keeps its rule: no light sleep until the radio is torn
down. (Later, if measurements allow: µA-cheap advertising could continue
during doze — a deliberate phase-2 experiment, not the default.)

## Framing: fitting frames through an MTU straw

BLE writes/notifies cap at the negotiated MTU (iOS grants ~185 bytes
typically). Every message rides length-prefixed frames; anything bigger
is fragmented and reassembled. Same framing both directions.

```text
frame  :=  [ len:u16 ] [ flags:u8 ] [ stream:u8 ] [ payload ≤ MTU-7 ]

flags  :   bit0-1  kind      00 = JSON message   01 = binary chunk
           bit2-3  position  00 = only frame     01 = first
                             10 = continuation   11 = last
stream :   reassembly lane (interleave bulk transfer with control chat)
```

- **kind=JSON**: reassembled frames concatenate into one UTF-8 JSON doc.
- **kind=binary**: raw chunk of an in-flight file transfer (no JSON tax
  on audio).
- Two streams are enough: `0` control, `1` bulk.

## Envelope: three message shapes, one initiator

JSON-RPC-flavored, minus everything a single-initiator design doesn't
need. The watch keeps one small table of its own outstanding requests
(id → timeout); the phone answers statelessly.

```json
// event — fire-and-forget, no reply expected
{ "ev": "telemetry", "data": { "steps": 4211, "bat": 87 } }

// request — watch asks, phone must answer this id
{ "id": 7, "m": "inbox.drain", "p": { "max": 10 } }

// response — the only thing a phone may ever send unprompted-looking
{ "id": 7, "ok": [ { "kind": "msg", "body": "..." } ] }
{ "id": 7, "err": { "code": "http_502", "msg": "upstream down" } }
```

Rules that keep it boring: every request has a **timeout** (default 10s);
every method is **idempotent** (re-asking is always safe — the outbox
drain acks by item id, so a lost response never loses a message).

## The verbs (v1: exactly four)

| Verb | Shape | Phone does |
|---|---|---|
| `telemetry` | event | POST to the manifest's telemetry route |
| `inbox.drain` | request | GET outbox route, return items; watch acks item ids in the next drain |
| `upload.begin / .end` | request | Open/close a staged file; on `.end`, verify sha and POST to upload route |
| *(binary frames)* | stream 1 | Append chunks to the staged file, ack every K chunks |

## Call stacks, visually

### Boot of a session: connect, read manifest, drain

```mermaid
sequenceDiagram
    participant W as watch
    participant P as phone app
    participant S as server
    Note over W: screen on → advertise
    P->>W: connect + MTU negotiation
    P->>W: read MANIFEST
    W-->>P: {routes, device_id, proto:1}
    W->>P: {id:1, m:"inbox.drain"}
    P->>S: GET /outbox?device=…&ack=[]
    S-->>P: [ {item 41}, {item 42} ]
    P-->>W: {id:1, ok:[41,42]}
    Note over W: next drain acks [41,42] → server deletes
```

### Telemetry: fire and forget

```mermaid
sequenceDiagram
    participant W as watch
    participant P as phone (may be backgrounded)
    participant S as server
    W->>P: notify {ev:"telemetry", data:{steps,bat}}
    Note over P: iOS wakes app ~10s (bluetooth-central)
    P->>S: POST /telemetry  (auth from keychain)
    Note over W: no reply expected; loss is fine,<br/>next telemetry supersedes
```

### Voice clip upload, with a mid-transfer connection drop

```mermaid
sequenceDiagram
    participant W as watch
    participant P as phone
    participant S as server
    W->>P: {id:9, m:"upload.begin", p:{name,size,sha}}
    P-->>W: {id:9, ok:{transfer:3, offset:0}}
    loop chunks on stream 1
        W->>P: binary frame (transfer 3)
        P-->>W: ack every K chunks {t:3, have:bytes}
    end
    Note over W,P: ✗ connection drops at 60%
    Note over W: screen still on → re-advertise
    P->>W: reconnect
    W->>P: {id:10, m:"upload.begin", p:{name,size,sha}}
    P-->>W: {id:10, ok:{transfer:3, offset:61440}}
    Note over P: same sha → resume, not restart
    W->>P: remaining chunks
    W->>P: {id:11, m:"upload.end", p:{t:3}}
    P->>P: verify sha
    P->>S: POST /uploads (file)
    S-->>P: 201
    P-->>W: {id:11, ok:true}
    Note over W: only NOW delete the local copy
```

### Server wants to tell the watch something

```mermaid
sequenceDiagram
    participant S as server
    participant P as phone
    participant W as watch
    S->>S: enqueue item in outbox (that's all it can do)
    Note over S,W: …time passes; nobody can push…
    W->>P: {id:12, m:"inbox.drain"}  (screen-on or N-min timer)
    P->>S: GET /outbox
    S-->>P: [item]
    P-->>W: {id:12, ok:[item]}
```

Downstream latency is bounded by the watch's ask cadence — by design.
"Now" doesn't exist for a device whose radio sleeps.

## Transfer state machine (watch side)

```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> Offering: clip recorded (staged in PSRAM/flash)
    Offering --> Sending: upload.begin ok (offset o)
    Sending --> Sending: chunk acks advance
    Sending --> Offering: connection lost (keep file)
    Sending --> Verifying: upload.end sent
    Verifying --> Idle: ok → delete local copy
    Verifying --> Offering: err → retry later
    Offering --> Idle: gave up (file kept, retry next session)
```

## The manifest

Authored in firmware, served over GATT, versioned with the protocol.
The phone resolves `auth` names against its keychain — **the manifest is
not secret** (it ships in a firmware binary); tokens never leave the
phone.

```json
{
  "proto": 1,
  "device": "pikachu-01",
  "routes": {
    "telemetry": { "post": "https://api.frolic.dev/pet/telemetry", "auth": "frolic" },
    "inbox":     { "get":  "https://api.frolic.dev/pet/outbox",    "auth": "frolic" },
    "uploads":   { "post": "https://api.frolic.dev/pet/uploads",   "auth": "frolic" }
  },
  "hosts_allow": ["api.frolic.dev"]
}
```

`hosts_allow` is the relay's safety rail: the engine refuses any route
outside it, so a generic public version of the app can't be turned into
an open proxy by a hostile device.

## Budgets and constraints

| Concern | Number | Note |
|---|---|---|
| NimBLE stack RAM | ~25-30KB | vs ~40-70KB free heap today → needs a memory pass first |
| Throughput (GATT, iOS) | ~5-50 KB/s | 30s voice clip ≈ 60-100KB ≈ seconds-to-half-minute |
| iOS background wake | ~10s per BLE event | every phone action must fit one slice |
| Advertising cost | tens of µA | phase-2 candidate: keep advertising while dozing |
| Frame payload | MTU−7 (~178B) | after iOS's typical 185-byte MTU grant |

## Phasing

```mermaid
flowchart LR
    P0["P0 · wifi power-save A/B\n(may defer this whole track)"]
    P1["P1 · firmware GATT + framing\nvalidated with nRF Connect,\nzero app code"]
    P2["P2 · Expo relay engine\n4 verbs, manifest-driven"]
    P3["P3 · generalize?\npublish app, open manifest"]
    P0 --> P1 --> P2 --> P3
```

- **P0** — flip `WIFI_PS_NONE` → modem power-save (truce-era relic),
  measure with the AXP2101 fuel gauge. If wifi gets cheap enough, BLE
  waits.
- **P1** — NimBLE peripheral + manifest + framed channel in firmware;
  drive it entirely from nRF Connect. The watch side is done before any
  app exists.
- **P2** — Expo dev-build app (`react-native-ble-plx`), relay engine
  with the four verbs, keychain auth, background mode.
- **P3** — only if wanted: the app was generic all along; open it up.

## Open questions

1. **BLE while dozing?** Advertising is µA-cheap, but the manual
   light-sleep loop and the BT controller's sleep clocking need a
   deliberate experiment (same rigor as the render harness).
2. **Chunk size / ack window (K)** — tune on the bench with nRF Connect
   throughput tests before freezing the protocol.
3. **Pairing/bonding** — v1 can rely on the manifest's allowlist + app
   pinning by device id; decide whether BLE-level bonding is worth its
   iOS UX friction.
4. **Coexistence** — wifi and BLE share the 2.4GHz radio. v1: BLE
   replaces wifi in the screen-on slot. Running both is a measured
   experiment, not an assumption.
