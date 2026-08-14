# pocket pet

A Pocket-Pikachu-style virtual-pet watch for the Waveshare
ESP32-S3-Touch-AMOLED-2.06 dev kit. A pixel pet wanders a GBA-style grass
field, counts your real steps, sleeps when the screen sleeps, wakes with a
stretch and yawn, and levels up as you walk. Simulator-first (LVGL SDL sim on
macOS); the same portable `app/` code runs on device.

## Layout

- `app/` — portable UI + game logic (pet FSM, watchface, step/game loop).
  Runs in both the sim and on device.
- `sim/` — SDL LVGL simulator (fast iteration on Mac).
- `device/` — ESP-IDF firmware (main app, BLE relay client, power, drivers,
  vendored Waveshare BSP).
- `tools/` — asset generators (sprites/HUD/font → C arrays) and
  `watch_remote.py` (serial screenshots + synthetic taps).
- `assets/<pokemon>/` — source PMD sprite sheets (the only sprite source in
  git; C arrays are generated at build).
- `.known-good/` — preserved clean binaries for one-flash recovery.

## Build & run

Sim:
```bash
cmake -B build -S . && cmake --build build && ./build/frolic_sim
```

Device (ESP-IDF ~5.4):
```bash
source ~/esp/esp-idf/export.sh
cd device
idf.py build
idf.py -p /dev/cu.usbmodem2101 flash
```
The watch is BLE-only: it reaches the internet through the
[Familiar](https://github.com/frolic/familiar) relay app on a phone.
`FROLIC_DISABLE_BLE=1` builds a radio-quiet bench firmware.

## Swapping the character

Drop a new PMD sprite folder in `assets/`, point the `POKEMON` CMake var (sim)
or the `assets/raichu` path in `device/main/CMakeLists.txt` (device) at it, and
rebuild. Sprites emit a stable `pet_` prefix, so no code changes are needed.

## Notes for contributors (and AI sessions)

See **`CLAUDE.md`** for hardware landmines learned the hard way — most
importantly: **the flush pipeline must wait for real SPI completion** (the
vendored BSP's fault-tolerant flush provides this; without it the draw
buffer is reused mid-DMA and the panel stripes/tears). Also: never touch
GPIO13, and the battery keeps the panel powered so USB-unplug does not
reset latched panel state.

