#pragma once

/* Leash BLE peripheral (P1 bench scope): advertises continuously, serves
   INFO, and runs the framed duplex channel — echoing JSON requests and
   emitting real telemetry events while a central is subscribed. See
   docs/ble-gateway-design.md. */
void device_leash_init(void);
