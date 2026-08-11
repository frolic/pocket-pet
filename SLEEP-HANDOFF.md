# Light-sleep session handoff (2026-08-11 19:45)

You are a fresh session with ONE mission: **make light sleep work on this
watch.** Read `CLAUDE.md` first (hardware landmines). This file is the
state handoff from the prior session.

## Kevin's mandate — the operating rule

**FIX FORWARD. Do not revert to a known-good build when something breaks.**
The prior session reverted three separate times when light-sleep attempts
misbehaved; Kevin explicitly and angrily forbade this. When the sleep build
breaks: debug it, fix it, reflash it. The known-good binaries
(`.known-good/`) exist only for a true bricked-device emergency, not as a
retreat. Kevin accepts a glitchy watch during this work — what he does not
accept is abandoning the problem.

## Current state

- **Device** (`/dev/cu.usbmodem2101`, plugged into USB): currently flashed
  with `.known-good/clean-full-featured.bin` (the last revert — sorry).
  First action: reflash the sleep build from source and start debugging.
- **`main` @ `d0195fc`**: the light-sleep implementation (manual
  `esp_light_sleep_start` loop in `device/main/device_sleep.c`), authored by
  a prior agent. Its design notes are in CLAUDE.md ("Light sleep" section).
  Key architecture: manual sleep loop engaged only in DOZING-on-battery,
  40ms sleep quanta with accel sampling each wake
  (`step_source_sample_now`), BOOT (GPIO0) gpio-wakeup for instant wake,
  PWR polled every 4th wake, `xTaskCatchUpTicks` to credit slept time,
  flush gate closed around the whole dark loop.
  `CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND` is OFF (critical — its startup
  hook floats every pad on first sleep entry and latches the panel).

## The open field regression (your first target)

Kevin powered on the d0195fc build (USB-plugged, so the sleep loop should
have been fully inert via the VBUS gate) and got:

- Stuck **"SYNCING"** banner for 13+ minutes — the 60-second stuck-radio
  watchdog in `device_state.c` (housekeeping task) **never fired**.
- Scrambled sprite + fully-black battery icon — a corrupted frame burned
  during the boot/sync phase (`flushfail=333` accumulated, then `(+0)` —
  not climbing; frozen bad frame, not active failure).
- `psram free 3511k` vs ~4737k on the pre-sleep build (~1.2MB extra PSRAM
  held — find out what).

Questions to answer: why didn't the watchdog fire (did the tick catch-up /
`step_source_external_pacing` changes stall the housekeeping task even on
USB)? Why did boot accumulate 333 flush drops with visible corruption? Is
the sleep task truly inert on VBUS?

## Verification bar

- 5+ consecutive power-on cycles reaching ACTIVE, no stuck states, visually
  clean (Kevin's eyes for the final call — snapshots can't see panel-level
  corruption, per CLAUDE.md).
- Then Kevin's on-battery protocol (also in prior commit message /
  CLAUDE.md): unplug → fade → BOOT wake (~¼s) → PWR wake (~½s) → steps
  count while dark → overnight drain (expect single-digit % over ~8h).

## Practical notes

- Build: `cd device && source ~/esp/esp-idf/export.sh && idf.py build &&
  idf.py -p /dev/cu.usbmodem2101 flash`. No serial monitors running; the
  port is free. Kill any `python3 ... serial` stragglers before flashing.
- Serial heartbeat: `HB up=... flushfail=... wifi=... steps=... bat=...`
  every 2s. Debug console commands: `snap`, `wake`, `sleep`, `tap X Y`,
  `time Y M D h m`, `portal`, `portalx`.
- Kevin is present this evening and can do eyes-on-glass checks and
  on-battery tests when you ask — ask for specific, single observations.
- Commit and push (origin = github.com/frolic/pocket-raichu, private) as
  you go. Wiki logging is handled elsewhere; focus on the firmware.
