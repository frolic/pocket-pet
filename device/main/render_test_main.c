#ifdef FROLIC_RENDER_TEST
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "bsp/display.h"
#include "render_test_main.h"

/*
 * Render-characterization firmware (FROLIC_RENDER_TEST=1 idf.py build).
 *
 * Draws via esp_lcd directly — LVGL is never initialized — so every layer
 * of the normal stack (LVGL render, lvgl_port flush, flush gate) is out of
 * the picture and the panel + esp_lcd + SPI driver envelope is measured on
 * its own. Serial output is the deliverable: per-draw error codes, timing,
 * determinism across repeats, and correlation with radio load. The panel is
 * write-only (no RAMRD on this QSPI wiring), so a human glance at the final
 * glass state is the one non-serial verdict: after PASS the screen must
 * show an unbroken checkerboard with a white marker staircase.
 *
 * Phases:
 *   A baseline   full-screen fills, black/white/checker x repeats
 *   B sizes      strip heights 2..250, burst and paced full-screen sweeps
 *   C determinism height-50 burst x5, do the same strips fail every run?
 *   D stress     64 same-place draws back-to-back, where do errors start?
 *   E memory     identical sweeps from PSRAM vs internal-DMA buffers
 *   F radio      sweeps with wifi scanning continuously, then after stop
 *   G sustained  20s of random pet-sized partial draws (animation load)
 */

#define WIDTH BSP_LCD_H_RES
#define HEIGHT BSP_LCD_V_RES
#define MAX_STRIP 250

static uint16_t *psram_bufs[2];
static uint16_t *internal_buf; /* one strip, MALLOC_CAP_INTERNAL|DMA */
static esp_lcd_panel_handle_t panel;

static uint32_t total_draws;
static uint32_t total_errors;

/* Failure journal: geometry + error of every failed draw, for determinism
   analysis across repeated identical sweeps. */
typedef struct {
    const char *phase;
    int y;
    int height;
    esp_err_t err;
} failure_t;
#define MAX_FAILURES 512
static failure_t failures[MAX_FAILURES];
static int failure_count;

static esp_err_t draw(const char *phase, int y, int height, uint16_t *buf)
{
    int64_t start = esp_timer_get_time();
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, y, WIDTH, y + height, buf);
    int64_t took_us = esp_timer_get_time() - start;
    total_draws++;
    if (err != ESP_OK) {
        total_errors++;
        if (failure_count < MAX_FAILURES) {
            failures[failure_count++] = (failure_t){phase, y, height, err};
        }
        printf("RT-FAIL %s y=%d h=%d err=%s t=%lldus\n",
               phase, y, height, esp_err_to_name(err), (long long)took_us);
    } else if (took_us > 20000) {
        printf("RT-SLOW %s y=%d h=%d t=%lldus\n", phase, y, height, (long long)took_us);
    }
    return err;
}

static void fill_checker(uint16_t *buf, int base_y, int height, int index)
{
    for (int row = 0; row < height; row++) {
        for (int x = 0; x < WIDTH; x++) {
            bool checker = (((x >> 4) ^ ((base_y + row) >> 4)) & 1) != 0;
            buf[row * WIDTH + x] = checker ? 0xFFFF : 0x0000;
        }
    }
    /* Marker bar with a gap that steps right per strip index. */
    int gap_x = 8 + (index % 12) * 32;
    for (int row = height / 2 - 4; row < height / 2 + 4; row++) {
        if (row < 0 || row >= height) continue;
        for (int x = 0; x < WIDTH; x++) {
            bool in_gap = x >= gap_x && x < gap_x + 24;
            buf[row * WIDTH + x] = in_gap ? 0x0000 : 0xFFFF;
        }
    }
}

static void fill_solid(uint16_t *buf, int pixels, uint16_t value)
{
    for (int i = 0; i < pixels; i++) buf[i] = value;
}

/* One full-screen sweep in strips; returns the number of failed strips. */
static int sweep(const char *phase, int strip_height, int pace_ms, uint16_t *fixed_buf)
{
    int failed = 0;
    int index = 0;
    for (int y = 0; y < HEIGHT; y += strip_height, index++) {
        int height = strip_height;
        if (y + height > HEIGHT) height = HEIGHT - y;
        uint16_t *buf = fixed_buf != NULL ? fixed_buf : psram_bufs[index % 2];
        fill_checker(buf, y, height, index);
        if (draw(phase, y, height, buf) != ESP_OK) failed++;
        if (pace_ms > 0) vTaskDelay(pdMS_TO_TICKS(pace_ms));
    }
    return failed;
}

static void phase_a_baseline(void)
{
    printf("RT== phase A: baseline full-screen fills\n");
    for (int repeat = 0; repeat < 9; repeat++) {
        /* Single buffer is safe here: the 150ms pause outlasts the DMA. */
        uint16_t *buf = psram_bufs[0];
        if (repeat % 3 == 2) fill_checker(buf, 0, HEIGHT, 0);
        else fill_solid(buf, WIDTH * HEIGHT, repeat % 3 == 0 ? 0x0000 : 0xFFFF);
        draw("A-full", 0, HEIGHT, buf);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void phase_b_sizes(void)
{
    static const int heights[] = {2, 8, 20, 50, 120, 250};
    for (size_t i = 0; i < sizeof(heights) / sizeof(heights[0]); i++) {
        printf("RT== phase B: strips h=%d burst\n", heights[i]);
        int burst = sweep("B-burst", heights[i], 0, NULL);
        vTaskDelay(pdMS_TO_TICKS(300));
        printf("RT== phase B: strips h=%d paced 20ms\n", heights[i]);
        int paced = sweep("B-paced", heights[i], 20, NULL);
        printf("RT-B h=%d burst_failed=%d paced_failed=%d\n", heights[i], burst, paced);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

static void phase_c_determinism(void)
{
    printf("RT== phase C: determinism, h=50 burst x5\n");
    uint32_t seen[5][12] = {0};
    for (int run = 0; run < 5; run++) {
        int before = failure_count;
        sweep("C-det", 50, 0, NULL);
        for (int f = before; f < failure_count; f++) {
            int strip = failures[f].y / 50;
            if (strip < 12) seen[run][strip] = 1;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    printf("RT-C per-run failed strips:\n");
    for (int run = 0; run < 5; run++) {
        printf("RT-C run %d:", run);
        for (int s = 0; s < 12; s++) if (seen[run][s]) printf(" %d", s);
        printf("\n");
    }
}

static void phase_d_stress(void)
{
    printf("RT== phase D: 64 back-to-back draws, h=20, same y\n");
    fill_checker(psram_bufs[0], 0, 20, 0);
    fill_checker(psram_bufs[1], 0, 20, 1);
    int first_fail = -1;
    for (int i = 0; i < 64; i++) {
        if (draw("D-stress", 240, 20, psram_bufs[i % 2]) != ESP_OK && first_fail < 0) {
            first_fail = i;
        }
    }
    printf("RT-D first_fail_at=%d\n", first_fail);
}

static void phase_e_memory(void)
{
    printf("RT== phase E: PSRAM vs internal-DMA buffer, h=50 burst x3\n");
    for (int run = 0; run < 3; run++) {
        int psram_failed = sweep("E-psram", 50, 0, NULL);
        vTaskDelay(pdMS_TO_TICKS(200));
        int internal_failed = internal_buf != NULL ? sweep("E-int", 50, 0, internal_buf) : -1;
        printf("RT-E run %d psram_failed=%d internal_failed=%d\n",
               run, psram_failed, internal_failed);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static volatile bool radio_scanning;

static void scan_task(void *arg)
{
    (void)arg;
    while (radio_scanning) {
        wifi_scan_config_t scan_config = {0};
        esp_wifi_scan_start(&scan_config, true); /* blocking ~2s all-channel */
        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);
        printf("RT-radio scan done, %u APs\n", count);
    }
    vTaskDelete(NULL);
}

static bool wifi_bring_up(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init_config) != ESP_OK ||
        esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        return false;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    return true;
}

static void phase_f_radio(void)
{
    printf("RT== phase F: sweeps under active wifi scanning\n");
    if (!wifi_bring_up()) {
        printf("RT-F wifi unavailable, skipping\n");
        return;
    }
    radio_scanning = true;
    xTaskCreate(scan_task, "rtscan", 4096, NULL, 5, NULL);
    for (int run = 0; run < 3; run++) {
        int failed = sweep("F-radio", 50, 0, NULL);
        printf("RT-F radio run %d failed=%d\n", run, failed);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    radio_scanning = false;
    vTaskDelay(pdMS_TO_TICKS(2500)); /* let the last scan finish */
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(500));
    int after = sweep("F-after", 50, 0, NULL);
    printf("RT-F after radio stop failed=%d\n", after);
}

static void phase_g_sustained(void)
{
    printf("RT== phase G: 20s of random partial draws (animation load)\n");
    int64_t end = esp_timer_get_time() + 20 * 1000 * 1000;
    uint32_t draws = 0;
    uint32_t errors_before = total_errors;
    uint32_t seed = 12345;
    while (esp_timer_get_time() < end) {
        seed = seed * 1103515245 + 12345;
        int y = (int)((seed >> 16) % (HEIGHT - 80)) & ~1;
        uint16_t *buf = psram_bufs[draws % 2];
        fill_checker(buf, y, 80, (int)(draws % 12));
        draw("G-anim", y, 80, buf);
        draws++;
        vTaskDelay(pdMS_TO_TICKS(30)); /* ~33fps region updates */
    }
    printf("RT-G draws=%lu errors=%lu\n",
           (unsigned long)draws, (unsigned long)(total_errors - errors_before));
}

/*
 * Phase H: the app's real stack — LVGL + lvgl_port with the app's exact
 * buffer config — but with the flush callback replaced by an instrumented
 * wrapper that logs the errno and geometry of every draw_bitmap failure
 * (the app's storms never surfaced either). Sub-phases cross workloads to
 * find the trigger: plain repaints, +NVS commit bursts, +wifi scanning,
 * +brightness fades.
 */

static volatile bool nvs_stress_running;

static void nvs_stress_task(void *arg)
{
    (void)arg;
    nvs_handle_t handle;
    if (nvs_open("rtstress", NVS_READWRITE, &handle) != ESP_OK) {
        printf("RT-H nvs open failed\n");
        vTaskDelete(NULL);
        return;
    }
    uint32_t counter = 0;
    while (nvs_stress_running) {
        nvs_set_u32(handle, "c", counter++);
        nvs_commit(handle);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    nvs_close(handle);
    vTaskDelete(NULL);
}

static lv_obj_t *h_rect;
static volatile int h_frames_left;
static volatile bool h_fade;
static volatile uint32_t h_rect_color; /* phase label: red = radio live */
static lv_timer_t *h_frame_timer;

/* Runs on the LVGL task — same execution context as every real app render.
   Each tick forces a full-screen repaint: toggle the screen background and
   move a big rect; the display refresh timer flushes it. */
static void h_frame_tick(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (h_frames_left <= 0) return;
    int frame = h_frames_left--;
    lv_obj_set_style_bg_color(lv_screen_active(),
                              frame % 2 ? lv_color_hex(0x224422)
                                        : lv_color_hex(0x442244),
                              0);
    lv_obj_set_pos(h_rect, (frame * 7) % 200, (frame * 13) % 300);
    lv_obj_set_style_bg_color(h_rect, lv_color_hex(h_rect_color), 0);
    lv_obj_invalidate(lv_screen_active());
    /* The app's fade calls the brightness command from this task too. */
    if (h_fade) bsp_display_brightness_set(30 + (frame % 50));
}

static void h_frames(const char *load, int frames, bool fade, uint32_t color)
{
    printf("RT-H %s begin frames=%d\n", load, frames);
    h_fade = fade;
    h_rect_color = color;
    h_frames_left = frames;
    while (h_frames_left > 0) vTaskDelay(pdMS_TO_TICKS(100));
    printf("RT-H %s done\n", load);
}

static void phase_h_lvgl(void)
{
    printf("RT== phase H: LVGL stack with instrumented flush\n");
    if (!wifi_bring_up()) printf("RT-H wifi unavailable, H3/H4 will idle\n");
    /* Internal-DMA draw buffer: with real flush-wait semantics (vendored
       BSP) the buffer is never reused mid-DMA — probing whether the
       "internal buffer stripes the panel" landmine was only ever the
       missing wait. Also removes the per-flush internal bounce alloc whose
       ESP_ERR_NO_MEM is the app's flush-failure storm. */
    bsp_display_cfg_t display_config = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        }};
    display_config.lvgl_port_cfg.task_affinity = 1;
    lv_display_t *display = bsp_display_start_with_config(&display_config);
    if (display == NULL) {
        printf("RT-H display start failed\n");
        return;
    }
    bsp_display_brightness_set(80);
    bsp_display_lock(0);
    h_rect = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(h_rect);
    lv_obj_set_size(h_rect, 180, 140);
    lv_obj_set_style_bg_color(h_rect, lv_color_hex(0xDDAA33), 0);
    lv_obj_set_style_bg_opa(h_rect, LV_OPA_COVER, 0);
    h_frame_timer = lv_timer_create(h_frame_tick, 15, NULL);
    bsp_display_unlock();

    /* Rect color labels the phase on glass: yellow/green = radio idle,
       RED = radio actively scanning (watch hardest), white = fade test. */
    h_frames("H1-plain", 200, false, 0xDDAA33);

    nvs_stress_running = true;
    xTaskCreate(nvs_stress_task, "rtnvs", 3072, NULL, 4, NULL);
    h_frames("H2-nvs", 200, false, 0x33BB55);
    nvs_stress_running = false;
    vTaskDelay(pdMS_TO_TICKS(200));

    radio_scanning = true;
    xTaskCreate(scan_task, "rtscan2", 4096, NULL, 5, NULL);
    h_frames("H3-wifi", 400, false, 0xDD2222);

    nvs_stress_running = true;
    xTaskCreate(nvs_stress_task, "rtnvs2", 3072, NULL, 4, NULL);
    h_frames("H4-wifi-nvs", 400, false, 0xDD2222);
    nvs_stress_running = false;
    radio_scanning = false;
    vTaskDelay(pdMS_TO_TICKS(2500));

    h_frames("H5-fade", 200, true, 0xFFFFFF);
    bsp_display_brightness_set(80);
}

static void final_pattern(void)
{
    /* The one-glance verdict: full checkerboard + staircase, painted with a
       calm paced sweep. If any band is missing or misplaced, the panel lost
       data that esp_lcd reported as delivered. */
    sweep("final", 50, 30, NULL);
}

void render_test_main(void)
{
#if FROLIC_RENDER_TEST == 2
    printf("RENDER-TEST build mode 2: LVGL stack, instrumented flush\n");
    phase_h_lvgl();
    while (true) {
        printf("RENDER-TEST COMPLETE mode=2 (failures appear as 'flush retry'/'flush FAILED' lines)\n");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
#else
    printf("RENDER-TEST build mode 1: raw esp_lcd, LVGL not initialized\n");
    bsp_display_config_t display_config = {
        .max_transfer_sz = WIDTH * HEIGHT * sizeof(uint16_t),
    };
    esp_lcd_panel_io_handle_t io;
    ESP_ERROR_CHECK(bsp_display_new(&display_config, &panel, &io));
    bsp_display_brightness_set(80);

    psram_bufs[0] = heap_caps_malloc(WIDTH * HEIGHT * 2, MALLOC_CAP_SPIRAM);
    psram_bufs[1] = heap_caps_malloc(WIDTH * MAX_STRIP * 2, MALLOC_CAP_SPIRAM);
    internal_buf = heap_caps_malloc(WIDTH * MAX_STRIP * 2,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (psram_bufs[0] == NULL || psram_bufs[1] == NULL) {
        printf("RENDER-TEST: buffer alloc failed, aborting\n");
        return;
    }
    if (internal_buf == NULL) printf("RENDER-TEST: no internal buffer, phase E partial\n");

    phase_a_baseline();
    phase_b_sizes();
    phase_c_determinism();
    phase_d_stress();
    phase_e_memory();
    phase_f_radio();
    phase_g_sustained();
    final_pattern();

    while (true) {
        printf("RENDER-TEST COMPLETE draws=%lu errors=%lu failures_logged=%d\n",
               (unsigned long)total_draws, (unsigned long)total_errors, failure_count);
        for (int f = 0; f < failure_count && f < 40; f++) {
            printf("RT-J %s y=%d h=%d %s\n", failures[f].phase, failures[f].y,
                   failures[f].height, esp_err_to_name(failures[f].err));
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
#endif
}
#endif /* FROLIC_RENDER_TEST */
