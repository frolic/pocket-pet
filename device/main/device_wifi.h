#pragma once

#include <stdbool.h>

/*
 * Wifi with chat-stick style provisioning. Stored credentials (NVS) connect
 * as a station and keep the clock SNTP-synced (London time). No credentials
 * — or BOOT held during boot — starts an open "pocket-pikachu" access point
 * with a captive portal form that saves credentials and reboots.
 */
void device_wifi_start(void);

/* True when running the setup portal instead of normal station mode. */
bool device_wifi_in_portal(void);
