#pragma once

/*
 * Mechanical radio/display invariant: while closed, LVGL invalidation is
 * disabled — no rendering, no flushes. Must be closed before any
 * esp_wifi_start and opened only after esp_wifi_stop.
 */

void device_flush_gate_close(void);

void device_flush_gate_open(void);
