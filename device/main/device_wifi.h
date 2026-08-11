#pragma once

#include <stdbool.h>

/*
 * Wifi with chat-stick style provisioning. Stored credentials (NVS) connect
 * as a station and keep the clock SNTP-synced (London time). No credentials
 * — or BOOT held during boot — starts an open "pocket-pet" access point
 * with a captive portal form that saves credentials and reboots.
 */
void device_wifi_start(void);

/* True when running the setup portal instead of normal station mode. */
bool device_wifi_in_portal(void);

/* True while the radio is up (portal, or station until the post-sync stop). */
bool device_wifi_radio_active(void);

/* Opens a brief radio window (dev OTA / telemetry). True once connected. */
bool device_wifi_window_begin(uint32_t timeout_ms);

void device_wifi_window_end(void);

/* No known network was reachable at the last attempt. */
bool device_wifi_is_offline(void);

/* Emergency radio teardown for the stuck-state watchdog: stops the radio and
   resets window/active bookkeeping so future sync windows stay possible. */
void device_wifi_force_stop(void);

/* Enters the captive setup portal at runtime (wifi icon tap); no reboot. */
void device_wifi_request_portal(void);

/* Tears the portal down and returns to normal operation (cancel). */
void device_wifi_portal_exit(void);
