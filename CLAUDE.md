# CLAUDE.md — working notes for AI sessions on this repo

Read this before touching display, wifi, power, or GPIO code. It encodes
hard-won facts (mostly from a two-day debugging disaster) so they aren't
relearned the expensive way.

## Hardware landmines (each cost real time — do not relitigate)

### 1. The flush pipeline MUST wait for real SPI completion. (RESOLVED 2026-08-11 — read the history before touching the flush path)
The historic rule here ("draw buffer MUST live in PSRAM, internal-DMA
stripes the panel") was a MISDIAGNOSIS, fully characterized by the
FROLIC_RENDER_TEST harness. The true mechanism, three layers deep:

- The BSP registers this QSPI panel via `lvgl_port_add_disp_rgb` — RGB
  semantics call `lv_disp_flush_ready` immediately after queueing, so LVGL
  reused the draw buffer while the SPI DMA still read it. THAT was the
  stripe/tearing corruption with an internal buffer.
- A PSRAM buffer only looked clean because spi_master bounce-copies
  non-DMA-capable buffers through a contiguous internal-DMA malloc per
  flush — accidental copy semantics. That malloc failing under wifi heap
  fragmentation (`ESP_ERR_NO_MEM`, needs strip-size contiguous DMA heap)
  was the ENTIRE "spi transmit (queue) color failed" storm, with
  run-to-run variance = heap-fragmentation lottery.
- `panel_sh8601_draw_bitmap` swallows the color-transfer status (returns
  ESP_OK unconditionally), which is why no layer ever saw the errors.

Current architecture (vendored BSP `esp32_s3_touch_amoled_2_06.c`):
`bsp_flush_with_retry` drives CASET/RASET/RAMWR directly on the panel io
(real errors visible), retries ×4, and releases LVGL by hand on total
failure; a registered trans-done callback + `bsp_flush_wait_bounded`
(spin ≤5ms, yield after, give up at 200ms) give LVGL true wait semantics
so the draw buffer is never reused mid-DMA. With that, the draw buffer is
**internal DMA** (`buff_dma = true` in `device/main/main.c` — the BSP now
honors caller flags) — no bounce alloc, and bench `flushfail=0(+0)`,
which is the new baseline: ANY nonzero flushfail is a real regression.

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

### 4. "Radio corrupts the live display" — FALSIFIED 2026-08-11, same
misdiagnosis family as #1. Wifi's heap pressure triggered the bounce-alloc
NO_MEM flush failures, so radio activity correlated with corruption without
causing it. After the flush fix, the harness rendered full-speed animation
under continuous active scanning with zero driver-level failures AND
eyes-verified clean glass (mode 2, H3/H4, red-rect phases). Boot clock sync
now runs with live rendering (pet plays under the SETTING CLOCK banner).
The flush gate still exists for the cases where sealing is inherently
right: the manual sleep loop (nothing may be mid-SPI-transfer at
esp_light_sleep_start) and dark sync windows (screen is off anyway).

### 5. PSRAM runs at 40MHz (`CONFIG_SPIRAM_SPEED_40M`). Octal PSRAM on the
S3 caps at 80MHz; 40 was chosen for margin. Fine as-is.

## Boot, clock, and radio lifecycle

The PCF85063 RTC (0x51, battery-backed) makes the clock honest ~1.8s into
boot (`device_rtc_restore` in main); the pet scene boots live immediately
with no sync banner. The "SETTING CLOCK" banner sync only runs when the
RTC is invalid (first boot ever / battery pull / oscillator-stop) or when
fresh portal credentials need validating. Every SNTP completion writes the
RTC back.

**Radio follows the screen** (`radio_policy_task` in device_wifi.c): wifi
up + connected while the watch face is on (future uploads; SNTP drift
correction rides along), down when dark. The wifi fan icon shows the
truth: hidden = radio down and all well, pulsing yellow = seeking, white =
connected, red = offline or stranded (tap = portal). A failed connect goes
quiet until the next screen-on session. Sync windows (dozing, OTA) and the
portal own the radio themselves; `sleep_eligible` refuses to light-sleep
until the policy has torn the radio down. The stuck-radio watchdog
force-releases any banner sync stuck >60s (session clock — PWR presses
can't defer it), and the heartbeat's LVGL wedge detector restarts a frozen
pipeline outright.

## Debugging method that actually works here

**When the display corrupts: DIFF, don't theorize — then CHARACTERIZE.**
The 2025 saga was contained in one step by `git diff
<last-confirmed-clean-commit> HEAD` on the display files (it surfaced the
buff_dma/buff_spiram flip); the 2026-08-11 session then closed the root
cause for good by building the render-test harness below and measuring the
whole envelope instead of chasing symptoms (byte order, clock, strip
height, queue depth were all noise). Both lessons hold: diff first for
regressions, harness for anything the diff can't explain.

- `.known-good/clean-full-featured.bin` — flash to restore a known-clean,
  full-featured build:
  `python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem2101 -b 460800 write_flash 0x20000 .known-good/clean-full-featured.bin`
- **Render-characterization harness** (`device/main/render_test_main.c`):
  `FROLIC_RENDER_TEST=1 idf.py -B build_rt build` boots a raw esp_lcd
  battery (no LVGL — strip sweeps, bursts, determinism repeats, PSRAM vs
  internal, wifi load); `=2` boots the LVGL stack with the app's config
  under crossed loads (plain/NVS/wifi/fade). Serial-readable pass/fail per
  draw. This is how the flush saga was solved — extend it before theorizing
  about any new render fault.
- Remote debugging over USB serial (`tools/watch_remote.py`, and console
  commands in `device/main/device_debug_console.c`): `snap` (dump the
  logical framebuffer), `tap X Y`, `wake`/`sleep`, `portal`/`portalx`,
  `time`, `fill`, and raw panel draws bypassing LVGL (`rawfill [mode]`,
  `rawgrid [h] [ms]` — labeled strip patterns, `rawx` restores the app).
  **Caveat: snapshots show the LOGICAL frame LVGL composed, NOT panel-level
  corruption.** A clean snapshot with a striped panel means
  the fault is below LVGL (flush path / buffer placement / panel), so
  snapshots can't see #1-class bugs — only the human's eyes (or a camera)
  can. Don't conclude "clean" from a snapshot alone.

## Process (from both disasters — 2025 stripes and 2026-08-11 flush saga)

- **Do not declare the bug fixed until the human confirms with their eyes.**
  Panel-output bugs are invisible to every on-device counter and to
  framebuffer snapshots. "flushfail=0" and "no crashes" do NOT mean clean.
- **Two-strike rule.** Two failed (or partially-failed) fixes for the same
  symptom means the causal model is wrong. STOP patching. Build the
  instrument that can falsify the model (characterization firmware, raw
  draws, error-visible wrappers) before touching behavior again. In the
  flush saga, four symptom patches (boot cover ×2, sealed double-pass,
  heal repaints) burned hours; the harness found the three-layer root
  cause in ~90 minutes — and Kevin had to be the one to call for it.
- **A failed fix is data about the model, not just the fix.** Heal
  repaints failing PROVED the drops weren't transient — that falsified
  the model motivating them. Say out loud what a failure implies before
  proposing the next fix.
- **Make the fault visible before changing behavior.** This stack lied at
  three layers (sh8601 swallowed errors, RGB-mode faked flush-ready, the
  flushfail counter counted log strings). The first move against a
  recurring invisible fault is logging the error code and geometry at the
  failure site, not another mitigation.
- **Inherited landmine rules are compressed observations, not mechanisms.**
  "Draw buffer must be PSRAM" and "radio corrupts the display" were both
  false — and designing AROUND them (boot cover, radio truce choreography)
  built real complexity on top of misdiagnoses. When debugging inside a
  rule's territory, re-derive its mechanism before trusting it.
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
No code changes. Branding is generic "pocket pet."

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
6. **Flushfail context:** historically sporadic (0-100+/boot, variance =
   DMA-heap fragmentation lottery — see landmine #1 for the mechanism and
   its resolution). Since the fault-tolerant flush + internal draw buffer,
   the baseline is ZERO: any `flushfail`, `flush retry`, or `flush FAILED`
   line is a real regression, not noise.
7. **The 2026-08-11 "stuck SYNCING 13+ min, watchdog silent" field failure
   was NOT the sleep loop** (8/8 bench power-cycles of the same build were
   clean). Decomposition: a boot-paint flush storm burned a corrupt frame;
   sync never resolved so the gate stayed closed (hence `flushfail` frozen
   at `(+0)` — a closed gate attempts no flushes); and every PWR press
   flapped SYNC_VISIBLE↔SYNCING, which back then reset the stuck-radio
   watchdog clock per hop, deferring it indefinitely. Fixed: the watchdog
   clock now marks radio-session entry only. A `psram free` ~1.2MB below
   baseline is the `snap` console command's persistent buffers (RGB565 +
   ARGB8888 full-screen, allocated once, kept) — not a leak.
8. **A wedged SPI flush blocks the LVGL task forever** and with it every
   lv_timer (including the PWR-wake poll — dark screen becomes
   unwakeable) and every task that then takes the display lock (flush
   gate, watchdog recovery, sleep loop handback). No counter shows it:
   flushfail goes QUIET, not loud. The heartbeat's LVGL-liveness wedge
   detector (`device_debug.c`, fed by a 500ms lv_timer in `main.c`)
   restarts the device on stall — 10s leash normally, 2min in DOZING
   because manual light sleep slows LVGL to catch-up windows. Don't
   remove the liveness timer or gate it on state.

## Still open

- **On-battery verification of light sleep** (it is VBUS-gated off while
  plugged, and serial dies unplugged): overnight battery-drain wear test,
  step-counting-while-dark walk test, PWR/BOOT wake latency by feel.
