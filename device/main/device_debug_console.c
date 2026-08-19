#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "lvgl.h"
#include "esp_lcd_panel_ops.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "device_debug_console.h"
#include "device_rtc.h"
#include "device_debug.h"
#include "device_flush_gate.h"
#include "device_familiar.h"
#include "device_sleep.h"
#include "device_step_source.h"
#include "power_button.h"
#include "display_sleep.h"

/*
 * Line-based debug console on the USB serial for remote-driving the watch:
 *   snap        dump the logical frame (screen + top layer) as base64 PNG-able blobs
 *   tap X Y     inject a synthetic touch at panel coordinates
 * The snapshot is what LVGL believes it rendered — panel-level corruption is
 * not visible in it, but all UI logic is.
 */

#define SNAP_WIDTH 410
#define SNAP_HEIGHT 502

static SemaphoreHandle_t snap_done;
static lv_draw_buf_t screen_buf;
static lv_draw_buf_t top_buf;
static uint8_t *screen_mem;
static uint8_t *top_mem;
static volatile bool snap_ok;

static volatile int tap_x = -1;
static volatile int tap_y;
static volatile int tap_frames;

static void synthetic_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    if (tap_x >= 0 && tap_frames > 0) {
        data->point.x = tap_x;
        data->point.y = tap_y;
        data->state = LV_INDEV_STATE_PRESSED;
        tap_frames--;
        if (tap_frames == 0) tap_x = -1;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void wake_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    display_sleep_wake();
}

static void sleep_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    display_sleep_sleep_now();
}

static void run_in_lvgl(lv_timer_cb_t callback)
{
    bsp_display_lock(0);
    lv_timer_t *timer = lv_timer_create(callback, 10, NULL);
    lv_timer_set_repeat_count(timer, 1);
    bsp_display_unlock();
}

static void snap_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    snap_ok =
        lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565,
                                     &screen_buf) == LV_RESULT_OK &&
        lv_snapshot_take_to_draw_buf(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888,
                                     &top_buf) == LV_RESULT_OK;
    xSemaphoreGive(snap_done);
}

/* Every line carries its byte offset so the host can detect and report any
   dropped or mangled line instead of silently shearing the image. */
static void print_base64(const char *tag, const uint8_t *data, size_t size)
{
    printf("%s begin %u\n", tag, (unsigned)size);
    size_t offset = 0;
    unsigned char line[97];
    while (offset < size) {
        size_t chunk = size - offset > 72 ? 72 : size - offset;
        size_t written = 0;
        mbedtls_base64_encode(line, sizeof(line), &written, data + offset, chunk);
        line[written] = '\0';
        printf("@%u:%s\n", (unsigned)offset, line);
        offset += chunk;
        if ((offset & 0x1FFF) == 0) vTaskDelay(1);
    }
    printf("%s end\n", tag);
}

static void run_snap(void)
{
    if (screen_mem == NULL) {
        uint32_t screen_stride = lv_draw_buf_width_to_stride(SNAP_WIDTH, LV_COLOR_FORMAT_RGB565);
        uint32_t top_stride = lv_draw_buf_width_to_stride(SNAP_WIDTH, LV_COLOR_FORMAT_ARGB8888);
        screen_mem = heap_caps_malloc(screen_stride * SNAP_HEIGHT, MALLOC_CAP_SPIRAM);
        top_mem = heap_caps_malloc(top_stride * SNAP_HEIGHT, MALLOC_CAP_SPIRAM);
        if (screen_mem == NULL || top_mem == NULL) {
            printf("snap: psram alloc failed\n");
            return;
        }
        lv_draw_buf_init(&screen_buf, SNAP_WIDTH, SNAP_HEIGHT, LV_COLOR_FORMAT_RGB565,
                         screen_stride, screen_mem, screen_stride * SNAP_HEIGHT);
        lv_draw_buf_init(&top_buf, SNAP_WIDTH, SNAP_HEIGHT, LV_COLOR_FORMAT_ARGB8888,
                         top_stride, top_mem, top_stride * SNAP_HEIGHT);
        snap_done = xSemaphoreCreateBinary();
    }
    bsp_display_lock(0);
    lv_timer_t *timer = lv_timer_create(snap_cb, 10, NULL);
    lv_timer_set_repeat_count(timer, 1);
    bsp_display_unlock();
    if (xSemaphoreTake(snap_done, pdMS_TO_TICKS(3000)) != pdTRUE || !snap_ok) {
        printf("snap: failed\n");
        return;
    }
    print_base64("SNAPSCREEN", screen_buf.data, screen_buf.data_size);
    print_base64("SNAPTOP", top_buf.data, top_buf.data_size);
}

/*
 * Raw panel draw diagnostics: draw via esp_lcd directly, bypassing LVGL,
 * so panel-level faults can be isolated per screen strip with the human
 * reading the glass against the per-strip serial log.
 *   rawfill [M]      one full-screen draw_bitmap; M: 0 black, 1 white, 2 checker
 *   rawgrid [H] [MS] sweep in H-row strips, MS ms pause between (0 = burst);
 *                    each strip: checkerboard + a marker bar whose gap steps
 *                    right with the strip index (a staircase) — a dropped
 *                    strip is a missing stair, a displaced strip is a stair
 *                    out of line
 *   rawx             reopen the flush gate (restore the app's rendering)
 * Patterns are pure black/white: immune to the RGB565 byte swap the normal
 * flush path applies, so raw draws need no swap.
 */

#define RAW_MAX_STRIP 250 /* two ping-pong halves of a full-frame buffer */

static uint16_t *raw_buf;

static bool raw_ready(void)
{
    if (raw_buf == NULL) {
        raw_buf = heap_caps_malloc(SNAP_WIDTH * SNAP_HEIGHT * 2, MALLOC_CAP_SPIRAM);
        if (raw_buf == NULL) {
            printf("raw: psram alloc failed\n");
            return false;
        }
    }
    /* Seal LVGL out of the panel and hold the display awake: without the
       hold, the 10s inactivity fade blanks the glass mid-inspection. */
    device_flush_gate_close();
    bsp_display_lock(0);
    display_sleep_set_hold(true);
    lv_display_trigger_activity(NULL);
    bsp_display_unlock();
    bsp_display_brightness_set(80);
    return true;
}

static void raw_fill(int mode)
{
    if (!raw_ready()) return;
    for (int y = 0; y < SNAP_HEIGHT; y++) {
        for (int x = 0; x < SNAP_WIDTH; x++) {
            uint16_t pixel;
            if (mode == 0) pixel = 0x0000;
            else if (mode == 1) pixel = 0xFFFF;
            else pixel = (((x >> 4) ^ (y >> 4)) & 1) ? 0xFFFF : 0x0000;
            raw_buf[y * SNAP_WIDTH + x] = pixel;
        }
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(bsp_display_get_panel_handle(),
                                              0, 0, SNAP_WIDTH, SNAP_HEIGHT,
                                              raw_buf);
    printf("rawfill mode=%d err=%s\n", mode, esp_err_to_name(err));
}

static void raw_grid(int strip_height, int pace_ms)
{
    if (strip_height < 2) strip_height = 2;
    if (strip_height > RAW_MAX_STRIP) strip_height = RAW_MAX_STRIP;
    strip_height &= ~1; /* panel wants even row alignment */
    if (!raw_ready()) return;

    int failures = 0;
    int index = 0;
    for (int y = 0; y < SNAP_HEIGHT; y += strip_height, index++) {
        int height = strip_height;
        if (y + height > SNAP_HEIGHT) height = SNAP_HEIGHT - y;
        /* Ping-pong halves: the previous strip may still be in DMA flight
           while this one is being filled. */
        uint16_t *strip = raw_buf + (index % 2) * SNAP_WIDTH * RAW_MAX_STRIP;
        for (int row = 0; row < height; row++) {
            for (int x = 0; x < SNAP_WIDTH; x++) {
                bool checker = (((x >> 4) ^ ((y + row) >> 4)) & 1) != 0;
                strip[row * SNAP_WIDTH + x] = checker ? 0xFFFF : 0x0000;
            }
        }
        /* Marker bar: solid white, with a black gap stepping right per strip. */
        int gap_x = 8 + (index % 12) * 32;
        for (int row = height / 2 - 4; row < height / 2 + 4; row++) {
            if (row < 0 || row >= height) continue;
            for (int x = 0; x < SNAP_WIDTH; x++) {
                bool in_gap = x >= gap_x && x < gap_x + 24;
                strip[row * SNAP_WIDTH + x] = in_gap ? 0x0000 : 0xFFFF;
            }
        }
        esp_err_t err = esp_lcd_panel_draw_bitmap(bsp_display_get_panel_handle(),
                                                  0, y, SNAP_WIDTH, y + height,
                                                  strip);
        if (err != ESP_OK) {
            failures++;
            printf("rawgrid strip %d y=%d..%d err=%s\n",
                   index, y, y + height, esp_err_to_name(err));
        }
        if (pace_ms > 0) vTaskDelay(pdMS_TO_TICKS(pace_ms));
    }
    printf("rawgrid done: strips=%d failures=%d height=%d pace=%dms\n",
           index, failures, strip_height, pace_ms);
}

static void console_task(void *arg)
{
    (void)arg;
    char line[64];
    int length = 0;
    while (true) {
        uint8_t byte;
        /* Read the driver directly — stdin was bound before the driver
           existed, so getchar never delivers. */
        if (usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(100)) != 1) {
            continue;
        }
        int ch = byte;
        if (ch != '\n' && ch != '\r') {
            if (length < (int)sizeof(line) - 1) line[length++] = (char)ch;
            continue;
        }
        line[length] = '\0';
        length = 0;
        if (line[0] == '\0') continue;
        if (strcmp(line, "snap") == 0) {
            device_debug_set_quiet(true);
            run_snap();
            device_debug_set_quiet(false);
        } else if (strcmp(line, "wake") == 0) {
            run_in_lvgl(wake_cb);
            printf("wake: ok\n");
        } else if (strcmp(line, "sleep") == 0) {
            run_in_lvgl(sleep_cb);
            printf("sleep: ok\n");
        } else if (strncmp(line, "time ", 5) == 0) {
            int year, month, day, hour, minute;
            if (sscanf(line + 5, "%d %d %d %d %d", &year, &month, &day, &hour, &minute) == 5) {
                struct tm local = {
                    .tm_year = year - 1900,
                    .tm_mon = month - 1,
                    .tm_mday = day,
                    .tm_hour = hour,
                    .tm_min = minute,
                };
                struct timeval now = {.tv_sec = mktime(&local)};
                settimeofday(&now, NULL);
                /* Persist to the battery-backed RTC: with no network sync,
                   this command is the only clock source after a cold boot. */
                printf("time: set, rtc %s\n",
                       device_rtc_store() ? "stored" : "store FAILED");
            }
        } else if (strncmp(line, "rawfill", 7) == 0) {
            int mode = 2;
            sscanf(line + 7, "%d", &mode);
            raw_fill(mode);
        } else if (strncmp(line, "rawgrid", 7) == 0) {
            int height = 50;
            int pace = 0;
            sscanf(line + 7, "%d %d", &height, &pace);
            raw_grid(height, pace);
        } else if (strncmp(line, "famping", 7) == 0) {
            unsigned count = 200;
            unsigned interval = 100;
            sscanf(line + 7, "%u %u", &count, &interval);
            device_familiar_ping(count, interval);
        } else if (strcmp(line, "weather") == 0) {
            device_familiar_weather();
        } else if (strcmp(line, "walklog") == 0) {
            device_debug_set_quiet(true);
            step_source_walklog_dump();
            device_debug_set_quiet(false);
        } else if (strcmp(line, "pmicrails") == 0) {
            power_rails_dump();
        } else if (strcmp(line, "pkeywatch") == 0) {
            power_button_watch();
        } else if (strcmp(line, "walkfile") == 0) {
            device_debug_set_quiet(true);
            step_source_walkfile_dump();
            device_debug_set_quiet(false);
        } else if (strcmp(line, "walkclear") == 0) {
            step_source_walklog_clear();
        } else if (strcmp(line, "pnlsleep") == 0) {
            /* Eyes test for SLPIN: seal rendering, put the panel to sleep. */
            device_flush_gate_close();
            printf("pnlsleep: %s\n",
                   esp_err_to_name(bsp_display_panel_sleep(true)));
        } else if (strcmp(line, "pnlwake") == 0) {
            printf("pnlwake: %s\n",
                   esp_err_to_name(bsp_display_panel_sleep(false)));
            device_flush_gate_open();
            run_in_lvgl(wake_cb);
        } else if (strcmp(line, "sleepstats") == 0) {
            device_sleep_stats_print();
        } else if (strcmp(line, "sleepstats reset") == 0) {
            device_sleep_stats_reset();
        } else if (strcmp(line, "i2cscan") == 0) {
            /* Definitive on-board sensor inventory (settles what the docs
               don't: ambient light sensor? battery-backed RTC?). */
            i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
            if (bus == NULL) {
                printf("i2cscan: bus not initialized\n");
            } else {
                for (uint8_t address = 0x08; address <= 0x77; address++) {
                    if (i2c_master_probe(bus, address, 50) == ESP_OK) {
                        printf("i2cscan: found 0x%02X\n", address);
                    }
                }
                printf("i2cscan: done\n");
            }
        } else if (strcmp(line, "rawx") == 0) {
            bsp_display_lock(0);
            display_sleep_set_hold(false);
            bsp_display_unlock();
            device_flush_gate_open();
            printf("rawx: app rendering restored\n");
        } else if (strncmp(line, "tap ", 4) == 0) {
            int x, y;
            if (sscanf(line + 4, "%d %d", &x, &y) == 2) {
                tap_x = x;
                tap_y = y;
                tap_frames = 4;
                printf("tap: injected %d,%d\n", x, y);
            }
        } else {
            printf("console: unknown '%s'\n", line);
        }
    }
}

void device_debug_console_start(void)
{
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    config.tx_buffer_size = 4096;
    usb_serial_jtag_driver_install(&config);

    bsp_display_lock(0);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, synthetic_read);
    bsp_display_unlock();

    xTaskCreate(console_task, "dbgcon", 6144, NULL, 3, NULL);
    printf("debug console ready (snap / tap X Y)\n");
}
