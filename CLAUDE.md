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

## Still open

- **Power management / full-day battery (goal 3): NOT done.** PM is currently
  OFF. `device_power.c` has the DFS + light-sleep plumbing but it's disabled.
  Re-enable carefully and verify on a real wear-test; the USJ pad must be
  dropped on battery for light sleep to engage (`device_power_set_full`),
  and GPIO13 stays untouched (#2). Do NOT bundle this with display changes.
