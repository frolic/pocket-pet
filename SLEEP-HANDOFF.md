# Session handoff (updated 2026-08-15)

Read `CLAUDE.md` first (hardware landmines — note landmine #1 was
rewritten after the flush saga; the old "PSRAM draw buffer" rule is dead).

## 2026-08-13/14: Familiar end-to-end MILESTONE + next work items

The BLE gateway is **Familiar** (github.com/frolic/familiar — pure Swift
iOS app, bundle id `tools.familiar.app`, Kevin owns familiar.tools;
protocol spec in its docs/design.md). Firmware side is
`device/main/device_familiar.c`: NimBLE peripheral advertising as
`pika`, INFO + framed TX/RX channel, telemetry relayed through the phone
to a real HTTP POST at iOS's worst-case mtu=23. Fitting BLE beside wifi
took a three-step internal-RAM campaign (wifi/lwip, NimBLE host, and
LVGL's 64KB pool all now in PSRAM) — heap is HEALTHIER than pre-BLE
(50k/44k-min with both radios up).

Measured latency (famping console command ↔ Familiar echo central):
~45ms RTT p50, 0/1200 loss, stable across foreground / background /
locked / post-SIGKILL state restoration, including 20ms streaming
cadence — background relay is viable for voice.

The relay engine is LIVE in the Swift app (FrameCodec / FrameReassembler
/ RelayEngine / DeviceCentral): telemetry events POST for real, and the
request/response path works end-to-end — the watch asks for London
weather (open-meteo, `weather` console command + once-a-minute demo in
famtel) and prints the temperature with an RTT decomposition (total =
phone http + ble/os). Measured: ~230ms warm via the relay vs 32ms warm /
552ms cold-TLS direct over wifi — and wifi could not open TCP at all
while a BLE central was connected (2.4GHz coexistence).

**2026-08-15: the watch went BLE-only.** Kevin picked BLE as the sole
transport; wifi/OTA/portal/SNTP were fully removed (git history before
this date has them). The state machine is two states (ACTIVE/DOZING),
BLE follows the screen (down when dark on battery, held up on USB), and
`sleep_eligible` refuses while a central is connected — the doze-kills-
the-session problem is closed. Binary shrank 667KB (fits factory with
689KB headroom); free heap roughly doubled (~145k).

**2026-08-15 late: the nighttime screen light-ups solved.** Field
breadcrumbs showed every dark loop ending in an RTC-watchdog reset
(~every 32min): manual light sleep under an active BLE controller
wedges the sleep entry/exit handshake (and burned ~23%/h keeping the RF
clock domain up). Fix: the radio policy stops the WHOLE NimBLE stack
(controller included, nimble_port_stop/deinit) when dark on battery,
restarts on screen-on. Verified: 178min of dark sleep across pocket +
bench with zero resets (old build never passed 32min); steps counted
while dark (1674 on a cycle+train commute). Boot-reason counters now
persist in sleepstats, and `FROLIC_SLEEP_ON_VBUS=1 idf.py -B
build_slpbench` builds the bench firmware that dark-sleeps on USB.
Bench fact: USB serial-JTAG does NOT survive wake-from-light-sleep —
only a full reset re-enumerates; a dark-sleeping watch is unreachable
until a PWR power-cycle (10s hold, then press).

Next firmware session:
1. Fresh-window dark-drain measurement with the controller-stop fix
   (`sleepstats reset`, overnight on battery) — expect single-digit %%/h.
2. Swap bring-up telemetry (httpbin.org) and the weather demo to the
   real pet API when it exists.

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

Watch out for: the draw buffer lives in internal RAM — if OOM symptoms
ever appear, the draw buffer height
(`CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT`/`BSP_LCD_RGB_BOUNCE_BUFFER_HEIGHT`)
is the sizing lever (BLE-only heap is ~145k free, so there is slack). A latched-panel state still survives reboots (landmine
#3): full 8-10s PWR hold is the reset.

## Practical notes

- Build: `cd device && source ~/esp/esp-idf/export.sh && idf.py build &&
  idf.py -p /dev/cu.usbmodem2101 flash`. ALWAYS cd with the absolute
  device/ path (cwd drift has polluted build dirs three times).
- Serial heartbeat every 2s; console: `snap`, `wake`, `sleep`, `tap X Y`,
  `time` (also stores the RTC), `sleepstats`, `famping`, `weather`,
  `rawfill`, `rawgrid`, `rawx`, `i2cscan`.
- Commit and push to main (origin = github.com/frolic/pocket-raichu,
  private) as you go. History note: commit `1e046a7` accidentally contains
  ~12k build-artifact files (untracked again in `4b38ae5a`); Kevin may
  approve a history rewrite to drop it.
