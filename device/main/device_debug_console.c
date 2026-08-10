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
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "device_debug_console.h"
#include "device_debug.h"
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
                printf("time: set\n");
            }
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
    usb_serial_jtag_driver_install(&config);

    bsp_display_lock(0);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, synthetic_read);
    bsp_display_unlock();

    xTaskCreate(console_task, "dbgcon", 6144, NULL, 3, NULL);
    printf("debug console ready (snap / tap X Y)\n");
}
