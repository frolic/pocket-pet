#pragma once

/* Familiar BLE peripheral (P1 bench scope): advertises continuously, serves
   INFO, and runs the framed duplex channel — echoing JSON requests and
   emitting real telemetry events while a central is subscribed. Protocol
   spec: docs/design.md in github.com/frolic/familiar. */
void device_familiar_init(void);

/* Latency rig: sends `count` timestamped binary frames at `interval_ms`,
   expects the central to echo them, prints the RTT distribution over
   serial. Blocks the calling task for count*interval + 3s grace. */
void device_familiar_ping(uint32_t count, uint32_t interval_ms);

/* Request/response demo: ask the relay for London's current temperature
   (open-meteo.com) and print the reading when the reply lands. */
void device_familiar_weather(void);

/* True while a central holds the BLE connection. Light sleep must refuse
   while true: manual esp_light_sleep_start freezes the BLE controller and
   kills the session. */
bool device_familiar_central_connected(void);
