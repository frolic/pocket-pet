# frolic

Virtual-pet watch firmware for the Waveshare ESP32-S3-Touch-AMOLED-2.06 (410×502 AMOLED, QMI8658 IMU with hardware pedometer). Pocket Pikachu spirit: a critter that lives on your wrist and reacts to your steps.

Development is simulator-first: the whole UI runs natively on macOS via LVGL's SDL backend at the watch's exact resolution, so look/feel iteration never waits on a flash cycle.

## Run the simulator

```sh
brew install cmake sdl2
cmake -B build
cmake --build build -j4
./build/frolic_sim
```

A 410×502 window opens: clock, goal ring, pet, live step count. The fake step source walks in bursts; click the pet to make it hop. `FROLIC_SMOKE=1 ./build/frolic_sim` runs ~3s and exits (build verification).

## Structure

```
app/   portable app logic — everything here must build for both sim and device
  frolic_app.c   entry point: builds UI, polls the step source
  watchface.c    layout: clock, goal ring, step count
  pet.c          the critter: idle bob, hop on click, reacts to steps
  step_source.h  interface the app polls for steps
sim/   macOS simulator target
  main.c              LVGL + SDL window at 410×502
  fake_step_source.c  simulated walker
```

The `app/` ↔ `sim/` split is the contract: app code only touches LVGL and the interfaces in `app/`, never SDL or hardware. The future device target (PlatformIO or ESP-IDF) supplies its own `main` plus a `step_source` reading the QMI8658 pedometer, and `app/` comes along unchanged.

## Sprites

The pet is currently drawn from LVGL primitives. To use real sprite art, convert PNGs with the [LVGL image converter](https://lvgl.io/tools/imageconverter) (LV_COLOR_FORMAT_RGB565A8 is a good fit) and swap the body construction in `pet.c` for an `lv_image`.

## Device notes

Target hardware: ESP32-S3R8, CO5300 display driver over QSPI, FT3168 touch, QMI8658 IMU, PCF85063 RTC, AXP2101 PMIC. Reference firmwares: [chat-stick](https://github.com/steveruizok/chat-stick) (provisioning + server architecture, targets the 1.8" sibling board) and [OLEDS3Watch](https://github.com/joaquimorg/OLEDS3Watch) (this exact board on ESP-IDF).
