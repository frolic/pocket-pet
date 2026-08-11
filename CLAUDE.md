# CLAUDE.md — working notes for AI sessions on this repo

Read this before touching display, wifi, power, or GPIO code. It encodes
hard-won facts (mostly from a two-day debugging disaster) so they aren't
relearned the expensive way.

## Hardware landmines (each cost real time — do not relitigate)

### 1. The draw buffer MUST live in PSRAM. This is the big one.
`buff_spiram = true, buff_dma = false` in the LVGL display config
(`device/main/main.c` AND the vendored BSP
`device/components/esp32_s3_touch_amoled_2_06/esp32_s3_touch_amoled_2_06.c`).

An **internal-DMA draw buffer** (`buff_dma = true, buff_spiram = false`)
produces persistent **horizontal green/white stripe corruption** on this
CO5300 QSPI AMOLED. This is the single cause of the "tearing/stripes" saga.
Do NOT switch the draw buffer to internal RAM to "fix" anything — it is the
poison, not the cure. esp-bsp issue #716 (PSRAM chunking) sounds like it
applies here but does NOT — a plausible upstream root cause is not proof
it's your root cause.

### 2. Never configure or drive GPIO13.
A community note (Waveshare issue #6) calls it an "AMOLED boost enable."
On this board, configuring GPIO13 *at all* (drive it, or even set it to
floating input) latches the panel into corruption. Leave it at power-on
default — no `gpio_config` for pin 13 anywhere.

### 3. The battery keeps the panel powered; unplugging USB does NOT reset it.
Latched panel/PMIC state survives: USB unplug, ESP32 reboot, and reflash.
Only a true cold cycle clears it — hold PWR ~8-10s for a full AXP2101
power-off (cuts panel rails), or disconnect the battery. When debugging,
remember "identical firmware, different display output across days" usually
means persistent *device* state your own earlier firmware set, not flaky
hardware.

### 4. Radio and the live display corrupt each other on this board — but that
was NOT the stripe cause (see #1). The flush-gate + device state machine
(`device_flush_gate.c`, `device_state.c`) enforce "radio only while the
screen is dark/static." Keep that invariant, but don't blame wifi for
display corruption without a diff-proven link. During the saga, "offline
builds also striped" because the real cause (#1) was in `main.c`
unconditionally — wifi looked guilty by false correlation.

### 5. PSRAM runs at 40MHz (`CONFIG_SPIRAM_SPEED_40M`). Octal PSRAM on the
S3 caps at 80MHz; 40 was chosen for margin. Fine as-is.

## Debugging method that actually works here

**When the display corrupts: DIFF, don't theorize.** The saga was solved in
one step by `git diff <last-confirmed-clean-commit> HEAD` on the display
files, which surfaced the buff_dma/buff_spiram flip. Do that FIRST, before
tuning byte order, clock, strip height, etc. — those were all noise.

- `.known-good/clean-full-featured.bin` — flash to restore a known-clean,
  full-featured build:
  `python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem2101 -b 460800 write_flash 0x20000 .known-good/clean-full-featured.bin`
- Remote debugging over USB serial (`tools/watch_remote.py`, and console
  commands in `device/main/device_debug_console.c`): `snap` (dump the
  logical framebuffer), `tap X Y`, `wake`/`sleep`, `portal`/`portalx`,
  `time`, `fill`. **Caveat: snapshots show the LOGICAL frame LVGL composed,
  NOT panel-level corruption.** A clean snapshot with a striped panel means
  the fault is below LVGL (flush path / buffer placement / panel), so
  snapshots can't see #1-class bugs — only the human's eyes (or a camera)
  can. Don't conclude "clean" from a snapshot alone.

## Process (from the same disaster)

- **Do not declare the bug fixed until the human confirms with their eyes.**
  Panel-output bugs are invisible to every on-device counter and to
  framebuffer snapshots. "flushfail=0" and "no crashes" do NOT mean clean.
- When the human reasons from physics you got wrong (e.g. "the battery keeps
  the panel alive"), believe them and update.
- When the human says "bisect against the known-good version," do that
  immediately — it beats any theory.

## Build / flash

- Device: `cd device && idf.py build && idf.py -p /dev/cu.usbmodem2101 flash`
  (source `~/esp/esp-idf/export.sh` first). `FROLIC_DISABLE_WIFI=1` builds
  the offline daily watch.
- Sim: `cmake -B build -S . && cmake --build build && ./build/frolic_sim`.
- **Always TaskStop any serial monitor before `idf.py flash`** — port
  conflict otherwise.
- After every flash, check for a crash loop (no repeated `Backtrace` /
  `rst:` in serial) before trusting it.

## Assets / swapping the character

Sprite/HUD/font C arrays are generated at CMake configure time by
`tools/gen_assets.py` from source PNGs in `assets/<pokemon>/` — the `.c`
files are gitignored, only sources + generators are tracked. Sprites emit a
stable `pet_` prefix, so swapping character = point the `POKEMON` CMake var
(sim) / `assets/raichu` path (device `main/CMakeLists.txt`) at a new folder.
No code changes. Repo is **private** because the PMD Pokémon sprites must
not be published; branding is generic "pocket pet."

## Light sleep (device_sleep.c) — how it works and why it's MANUAL

Three attempts at esp_pm automatic light sleep (tickless idle) failed; the
landing architecture is a **manual `esp_light_sleep_start` loop** owned by
`device_sleep.c`, engaged only when the state machine reports DOZING on
battery. Facts that must not be relearned:

1. **`CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND` must stay OFF.** Its startup
   hook (`esp_sleep_startup_init` in esp-idf `sleep_gpio.c`) arms hardware
   sleep-sel isolation on EVERY digital pad — QSPI panel bus, I2C, GPIO13 —
   so the first light-sleep entry (manual OR automatic) floats them all and
   latches the CO5300 into persistent corruption. This was the
   frozen-display failure of the second auto-sleep attempt (under tickless
   it also force-selects `PM_SLP_DISABLE_GPIO`). Cost of off: a >6V ESD
   pulse on an input pad during sleep can reset the chip — visible and
   recoverable, unlike the panel latch.
2. **Do not enable `CONFIG_FREERTOS_USE_TICKLESS_IDLE`.** Suspected of
   worsening the pre-existing sporadic SPI flush drops; more importantly,
   automatic sleep can engage anywhere the pm locks allow, which is
   unauditable on this board. Manual sleep needs neither tickless nor
   `light_sleep_enable=true` (esp_pm stays DFS-only, the verified config).
3. **Manual `esp_light_sleep_start` ignores esp_pm locks**, so the sleep
   loop must create its own safety: it closes the flush gate before its
   first sleep (no render → no flush → nothing on the panel bus mid-sleep)
   and takes one I2C read (the accel sample) right before each sleep, which
   serializes behind any in-flight I2C transaction.
4. **FreeRTOS ticks freeze during manual light sleep.** The loop credits
   slept time back via `xTaskCatchUpTicks` every ~1s and then yields ~10ms
   so due tasks (OTA scheduler, watchdogs, NVS persist, idle/TWDT feed)
   run. Without this, tick-based scheduling stretches ~25x.
5. **Steps keep counting** because the loop sleeps in 40ms quanta (the
   detector's sampling period) and calls `step_source_sample_now()` on each
   wake while the normal sampling task stands down
   (`step_source_external_pacing`). PWR is polled every 4th wake (~160ms
   latency); BOOT wakes instantly via `gpio_wakeup_enable(GPIO0, LOW)` —
   re-armed at every dark-loop entry because any later `gpio_config` on
   GPIO0 silently resets the wake trigger type.
6. **Flushfail context:** sporadic `spi transmit (queue) color failed`
   drops during rendering (boot paint, ACTIVE animation, DOZING-entry
   snap) are PRE-EXISTING with large run-to-run variance (0-100 in the
   first minute across identical builds). Don't attribute them to sleep
   work without an A/B with several runs per side.

## Still open

- **On-battery verification of light sleep** (it is VBUS-gated off while
  plugged, and serial dies unplugged): overnight battery-drain wear test,
  step-counting-while-dark walk test, PWR/BOOT wake latency by feel.
