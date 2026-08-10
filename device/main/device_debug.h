#pragma once

/*
 * Device observability: prints the reset reason at boot, then a 2s heartbeat
 * (uptime, internal/PSRAM heap, flush-failure counter, wifi mode). Also
 * rate-limits the panel driver's flush-failure log spam — the spam itself
 * starves the system — while counting every occurrence.
 */
void device_debug_start(void);

/* Total dropped display flushes since boot (tearing indicator). */
uint32_t device_debug_flush_failure_count(void);

/* Silences periodic serial prints (heartbeat etc.) during bulk dumps —
   interleaved lines corrupt the base64 snapshot stream. */
void device_debug_set_quiet(bool quiet);

bool device_debug_quiet(void);
