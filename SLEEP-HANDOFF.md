# Light-sleep session handoff (updated 2026-08-12, was 2026-08-11 19:45)

Mission: **make light sleep work on this watch.** Read `CLAUDE.md` first
(hardware landmines — note landmine #1 was rewritten after the flush saga
was solved; the old "PSRAM draw buffer" rule is dead).

## Kevin's mandate — the operating rule

**FIX FORWARD. Do not revert to a known-good build when something breaks.**
The known-good binaries (`.known-good/`) exist only for a true
bricked-device emergency. Kevin accepts a glitchy watch during this work —
what he does not accept is abandoning the problem.

## State after the 2026-08-11 evening session

- The "open field regression" (stuck SYNCING, silent watchdog, corrupt
  frozen frame) is fully explained and fixed — see CLAUDE.md landmine #1
  and the `main` history from `530757e` through the flush-architecture
  commit. Highlights: watchdog session clock (state flaps can't defer it),
  LVGL wedge self-heal, VBUS fail-toward-plugged, boot runs with live rendering (pet plays under the
  SETTING CLOCK banner), and above all the fault-tolerant flush + internal DMA draw
  buffer. Bench baseline is now `flushfail=0(+0)`; ANY flush failure line
  is a regression.
- Kevin eyes-confirmed clean rendering under active radio (harness red-rect
  phases), which falsified landmine #4 and unlocked the live boot.
- I2C scan (`i2cscan` console command): 0x18 ES8311 codec, 0x34 AXP2101,
  0x40 (unidentified), **0x51 PCF85063 RTC — present but UNUSED by
  firmware**, 0x6B QMI8658. No ambient light sensor. The RTC could remove
  the boot clock-sync wait for every boot where it holds valid time.
- The RTC is now ADOPTED (`device_rtc.c`): restore at boot makes the clock
  honest in ~1.8s and the banner sync skips itself; SNTP completions store
  back. The SETTING CLOCK banner only exists for first-boot/battery-pull/
  credential-validation boots.
- **Radio follows the screen** (`radio_policy_task`): wifi connected while
  the face is on (wifi fan icon: yellow pulse seeking → white connected,
  red offline/stranded, hidden when down-and-fine), torn down when dark.
  `sleep_eligible` waits for the teardown before light-sleeping.
- The manual sleep loop (`device_sleep.c`) itself was never the problem —
  its architecture is unchanged and still VBUS-gated off while plugged.
- Diagnostics that now exist: `FROLIC_RENDER_TEST=1|2` characterization
  firmware, `rawfill`/`rawgrid`/`rawx`/`i2cscan` console commands,
  `tools/cycle_harness.py`, LVGL wedge detector in the heartbeat.

## Battery: where things stand (2026-08-13 evening)

- Overnight test on `330b5837` gave ~8-12h battery life. Sleep-stats
  instrumentation was built in response (`sleepstats` console command,
  NVS-persisted; `sleepstats reset` clears).
- A 30-min instrumented battery session ACQUITTED the sleep loop:
  entries=1, duty=96%, fuel gauge saw zero %% across 21 dark minutes
  (bounds dark drain <3%/h). OTA windows (30-min cadence, ~15s radio)
  are negligible. Verdict: the battery went to AWAKE hours — dominated
  by `WIFI_PS_NONE` (~80-100mA whenever screen-on + connected).
- Fix shipped in `f07593b0`: associate at full power, drop to
  `WIFI_PS_MIN_MODEM` on GOT_IP (~10-20mA connected).
- **Stats now accumulate passively through normal use** — no protocol
  needed. At any charge, run `sleepstats` for the cumulative story.
  Kevin deferred the next deliberate overnight check.
- Watch-item, do not chase yet: sporadic `reason=2` (auth expire) on
  join, reproduced with PS off during association (pre-existing, not the
  PS change); bench sessions end at the 10s fade before the retry lands.

Original verification checklist (still to confirm when convenient):

1. Overnight %% drop with the PS fix (expect single-digit).
2. BOOT press → wake in ~¼s. PWR press → wake in ~½s (160ms poll + fade).
3. Clock correct (tick catch-up + RTC), steps persisted and sane.
4. Later, any walk: steps count while dark (40ms accel quanta).

Watch out for: heap min is ~29k since the draw buffer moved internal —
if OOM symptoms appear under wifi load, the draw buffer height
(`CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT`/`BSP_LCD_RGB_BOUNCE_BUFFER_HEIGHT`)
is the sizing lever. A latched-panel state still survives reboots (landmine
#3): full 8-10s PWR hold is the reset.

## Practical notes

- Build: `cd device && source ~/esp/esp-idf/export.sh && idf.py build &&
  idf.py -p /dev/cu.usbmodem2101 flash`. ALWAYS cd with the absolute
  device/ path (cwd drift has polluted build dirs three times).
- Serial heartbeat every 2s; console: `snap`, `wake`, `sleep`, `tap X Y`,
  `time`, `portal`, `portalx`, `rawfill`, `rawgrid`, `rawx`.
- Commit and push to main (origin = github.com/frolic/pocket-raichu,
  private) as you go. History note: commit `1e046a7` accidentally contains
  ~12k build-artifact files (untracked again in `4b38ae5a`); Kevin may
  approve a history rewrite to drop it.
