#pragma once

/* Leash BLE peripheral (P1 bench scope): advertises continuously, serves
   INFO, and runs the framed duplex channel — echoing JSON requests and
   emitting real telemetry events while a central is subscribed. See
   docs/ble-gateway-design.md. */
void device_leash_init(void);

/* Latency rig: sends `count` timestamped binary frames at `interval_ms`,
   expects the central to echo them, prints the RTT distribution over
   serial. Blocks the calling task for count*interval + 3s grace. */
void device_leash_ping(uint32_t count, uint32_t interval_ms);
