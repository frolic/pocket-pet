#pragma once

/*
 * Connects to wifi (when wifi_credentials.h exists) and keeps the clock
 * synced via SNTP, London timezone. No-op without credentials.
 */
void device_time_start(void);
