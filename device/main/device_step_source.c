#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <math.h>
#include <time.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/i2c_master.h"
#include "lvgl.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "step_source.h"
#include "device_debug.h"

/*
 * QMI8658 hardware pedometer. Register sequences ported from lewisxhe's
 * SensorLib (MIT) QMI8658 pedometer support. Thresholds follow its example's
 * bring-up profile (the datasheet walking profile rejects nearly everything):
 * 4G @ 125Hz, 80mg peak-to-peak, 60mg peak, 4-step entry filter.
 */

#define QMI_ADDRESS_PRIMARY 0x6B
#define QMI_ADDRESS_SECONDARY 0x6A
#define REG_WHOAMI 0x00
#define REG_CTRL1 0x02
#define REG_CTRL2 0x03
#define REG_CTRL7 0x08
#define REG_CTRL8 0x09
#define REG_CTRL9 0x0A
#define REG_CAL1_L 0x0B
#define REG_CAL1_H 0x0C
#define REG_CAL2_L 0x0D
#define REG_CAL2_H 0x0E
#define REG_CAL3_L 0x0F
#define REG_CAL3_H 0x10
#define REG_CAL4_L 0x11
#define REG_CAL4_H 0x12
#define REG_STATUS_INT 0x2D
#define REG_STEP_CNT_LOW 0x5A
#define REG_RESET 0x60

#define WHOAMI_VALUE 0x05
#define COMMAND_CONFIGURE_PEDOMETER 0x0D
#define COMMAND_ACK 0x00

static i2c_master_dev_handle_t device;
static bool ready;
static int status_code; /* 0 ok; 1 no chip; 2/3 config batch failed */

static esp_err_t write_register(uint8_t reg, uint8_t value)
{
    uint8_t buffer[2] = {reg, value};
    return i2c_master_transmit(device, buffer, 2, 100);
}

static esp_err_t read_registers(uint8_t reg, uint8_t *out, size_t length)
{
    return i2c_master_transmit_receive(device, &reg, 1, out, length, 100);
}

/* CTRL9 command protocol: write command, wait for the done bit, acknowledge,
   then wait for it to clear. Requires CTRL8 bit7 (handshake via STATUS_INT)
   to be set first — without it the done bit idles high and commands are
   acknowledged before they execute. */
static bool run_command(uint8_t command)
{
    if (write_register(REG_CTRL9, command) != ESP_OK) return false;
    bool done = false;
    for (int i = 0; i < 500 && !done; i++) {
        uint8_t status = 0;
        if (read_registers(REG_STATUS_INT, &status, 1) == ESP_OK && (status & 0x80)) {
            done = true;
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    if (!done) {
        printf("qmi8658: command 0x%02x never signalled done\n", command);
        return false;
    }
    if (write_register(REG_CTRL9, COMMAND_ACK) != ESP_OK) return false;
    for (int i = 0; i < 500; i++) {
        uint8_t status = 0;
        if (read_registers(REG_STATUS_INT, &status, 1) == ESP_OK && !(status & 0x80)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    printf("qmi8658: command 0x%02x ack never cleared\n", command);
    return false;
}

static bool probe(uint8_t address)
{
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &device) != ESP_OK) {
        return false;
    }
    uint8_t whoami = 0;
    if (read_registers(REG_WHOAMI, &whoami, 1) == ESP_OK && whoami == WHOAMI_VALUE) {
        return true;
    }
    i2c_master_bus_rm_device(device);
    device = NULL;
    return false;
}

static bool pedometer_init(void)
{
    bsp_i2c_init();
    if (!probe(QMI_ADDRESS_PRIMARY) && !probe(QMI_ADDRESS_SECONDARY)) {
        printf("qmi8658: not found on i2c bus\n");
        status_code = 1;
        return false;
    }
    uint8_t revision = 0;
    read_registers(0x01, &revision, 1);
    printf("qmi8658: revision=0x%02x\n", revision);

    write_register(REG_RESET, 0xB0);
    vTaskDelay(pdMS_TO_TICKS(20));

    write_register(REG_CTRL1, 0x40); /* register address auto-increment */
    write_register(REG_CTRL8, 0x80); /* CTRL9 handshake via STATUS_INT.7 */
    write_register(REG_CTRL2, 0x16); /* accel 4G range, 125Hz ODR */

    /* Pedometer configuration, two CAL-register batches (SensorLib recipe). */
    write_register(REG_CAL1_L, 50);   /* sample window */
    write_register(REG_CAL1_H, 0);
    write_register(REG_CAL2_L, 80);   /* peak-to-peak threshold, mg */
    write_register(REG_CAL2_H, 0);
    write_register(REG_CAL3_L, 60);   /* peak threshold, mg */
    write_register(REG_CAL3_H, 0);
    write_register(REG_CAL4_H, 0x01);
    write_register(REG_CAL4_L, 0x02);
    if (!run_command(COMMAND_CONFIGURE_PEDOMETER)) return false;

    write_register(REG_CAL1_L, 0x90); /* step timeout window: 400 (low byte) */
    write_register(REG_CAL1_H, 0x01); /* 400 = 0x0190 */
    write_register(REG_CAL2_L, 8);    /* step quiet time */
    write_register(REG_CAL2_H, 4);    /* entry count — filters lone bumps */
    write_register(REG_CAL3_L, 0);    /* precision */
    write_register(REG_CAL3_H, 1);    /* significant-step interval */
    write_register(REG_CAL4_H, 0x02);
    write_register(REG_CAL4_L, 0x02);
    if (!run_command(COMMAND_CONFIGURE_PEDOMETER)) { status_code = 3; return false; }

    write_register(REG_CTRL7, 0x01);  /* accelerometer on */
    uint8_t ctrl8 = 0;
    read_registers(REG_CTRL8, &ctrl8, 1);
    write_register(REG_CTRL8, ctrl8 | (1 << 4)); /* pedometer on */

    printf("qmi8658: pedometer running\n");
    return true;
}

/*
 * Software step detector. The QMI8658's on-die pedometer engine never
 * produced a count on this board even with verified-executed configuration
 * (CTRL9 handshake confirmed, reference thresholds), so steps are detected
 * here instead: 25Hz accel magnitude, EMA gravity baseline, positive-peak
 * detection gated to walking cadence, and an entry filter so lone bumps
 * don't count. The hardware counter is still read in diagnostics for
 * comparison should a future silicon revision start working.
 */

/* 4G range: 8192 counts per g. */
#define SW_SAMPLE_MS 40
#define SW_PEAK_THRESHOLD 900.0f    /* ~0.11g above baseline */
#define SW_MIN_STEP_MS 280          /* max cadence ~3.5 steps/s */
#define SW_MAX_STEP_MS 1000         /* min cadence 1 step/s */
#define SW_ENTRY_STEPS 4
#define SW_RESET_GAP_MS 1400

static volatile uint32_t sw_steps;
static nvs_handle_t steps_nvs;
static uint32_t active_day;     /* yyyymmdd the current count belongs to */
static uint32_t last_saved_steps;

/* Days only count once the clock is real (SNTP has landed at least once
   this power cycle; the RTC survives soft resets). */
static uint32_t today_yyyymmdd(void)
{
    time_t now = time(NULL);
    if (now < 1600000000) return 0; /* clock not synced yet */
    struct tm local;
    localtime_r(&now, &local);
    return (uint32_t)((local.tm_year + 1900) * 10000 +
                      (local.tm_mon + 1) * 100 + local.tm_mday);
}

/* Restores today's count across reboots; discards counts from other days. */
static void steps_restore(void)
{
    if (nvs_open("frolic", NVS_READWRITE, &steps_nvs) != ESP_OK) return;
    uint32_t stored_day = 0;
    uint32_t stored_steps = 0;
    nvs_get_u32(steps_nvs, "day", &stored_day);
    nvs_get_u32(steps_nvs, "steps", &stored_steps);
    active_day = today_yyyymmdd();
    /* Unsynced clock: assume the stored count is still today's. */
    if (stored_day != 0 && (active_day == 0 || stored_day == active_day)) {
        sw_steps = stored_steps;
        if (active_day == 0) active_day = stored_day;
    }
    last_saved_steps = sw_steps;
    printf("steps: restored %lu (day %lu)\n",
           (unsigned long)sw_steps, (unsigned long)active_day);
}

static void steps_persist_tick(void)
{
    uint32_t today = today_yyyymmdd();
    if (active_day == 0) active_day = today;
    if (today != 0 && active_day != 0 && today != active_day) {
        printf("steps: midnight reset (day %lu -> %lu)\n",
               (unsigned long)active_day, (unsigned long)today);
        sw_steps = 0;
        active_day = today;
        last_saved_steps = 1; /* force a save below */
    }
    if (steps_nvs == 0 || sw_steps == last_saved_steps) return;
    nvs_set_u32(steps_nvs, "day", active_day);
    nvs_set_u32(steps_nvs, "steps", sw_steps);
    nvs_commit(steps_nvs);
    last_saved_steps = sw_steps;
}

static void step_detect_task(void *arg)
{
    (void)arg;
    float baseline = 8192.0f;
    bool above = false;
    uint32_t last_peak_ms = 0;
    int entry_run = 0;
    int persist_countdown = 0;
    steps_restore();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SW_SAMPLE_MS));
        if (--persist_countdown <= 0) {
            persist_countdown = 30000 / SW_SAMPLE_MS; /* every 30s */
            steps_persist_tick();
        }
        if (!ready) continue;
        uint8_t buffer[6];
        if (read_registers(0x35, buffer, 6) != ESP_OK) continue;
        int16_t ax = (int16_t)(buffer[0] | (buffer[1] << 8));
        int16_t ay = (int16_t)(buffer[2] | (buffer[3] << 8));
        int16_t az = (int16_t)(buffer[4] | (buffer[5] << 8));
        float magnitude = sqrtf((float)ax * ax + (float)ay * ay + (float)az * az);
        baseline += 0.05f * (magnitude - baseline);
        float deviation = magnitude - baseline;
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        if (entry_run > 0 && now - last_peak_ms > SW_RESET_GAP_MS) {
            entry_run = 0;
        }
        if (!above && deviation > SW_PEAK_THRESHOLD) {
            above = true;
            uint32_t gap = now - last_peak_ms;
            if (gap >= SW_MIN_STEP_MS && gap <= SW_MAX_STEP_MS) {
                if (entry_run < SW_ENTRY_STEPS) {
                    entry_run++;
                    if (entry_run == SW_ENTRY_STEPS) {
                        sw_steps += SW_ENTRY_STEPS; /* credit the run-up */
                    }
                } else {
                    sw_steps++;
                }
            } else {
                entry_run = 1; /* rhythm broken: this peak starts a new run */
            }
            last_peak_ms = now;
        } else if (above && deviation < SW_PEAK_THRESHOLD * 0.5f) {
            above = false;
        }
    }
}

/*
 * Flight recorder: raw accel + step count sampled at 4Hz into a PSRAM ring
 * (~34 min), surviving USB unplug on battery. 'd' on the console dumps the
 * ring; 'z' clears it. Debug aid while step detection is being tuned.
 */
typedef struct {
    uint32_t ms;
    uint32_t steps;
    int16_t ax, ay, az;
} flight_sample_t;

#define FLIGHT_CAPACITY 8192
static flight_sample_t *flight_ring;
static volatile uint32_t flight_count;

static void flight_sample_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (!ready) continue;
        uint8_t sbuf[3] = {0};
        uint8_t abuf[6] = {0};
        if (read_registers(REG_STEP_CNT_LOW, sbuf, 3) != ESP_OK) continue;
        if (read_registers(0x35, abuf, 6) != ESP_OK) continue;
        flight_sample_t sample = {
            .ms = (uint32_t)(esp_timer_get_time() / 1000),
            .steps = ((uint32_t)sbuf[2] << 16) | ((uint32_t)sbuf[1] << 8) | sbuf[0],
            .ax = (int16_t)(abuf[0] | (abuf[1] << 8)),
            .ay = (int16_t)(abuf[2] | (abuf[3] << 8)),
            .az = (int16_t)(abuf[4] | (abuf[5] << 8)),
        };
        flight_ring[flight_count % FLIGHT_CAPACITY] = sample;
        flight_count++;
    }
}

static void flight_console_task(void *arg)
{
    (void)arg;
    while (true) {
        int ch = getchar();
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (ch == 'z') {
            flight_count = 0;
            printf("FLIGHT cleared\n");
        }
        if (ch != 'd') continue;
        uint32_t total = flight_count;
        uint32_t stored = total < FLIGHT_CAPACITY ? total : FLIGHT_CAPACITY;
        printf("FLIGHT begin total=%lu stored=%lu\n",
               (unsigned long)total, (unsigned long)stored);
        for (uint32_t i = total - stored; i < total; i++) {
            flight_sample_t *s = &flight_ring[i % FLIGHT_CAPACITY];
            printf("F %lu %lu %d %d %d\n", (unsigned long)s->ms,
                   (unsigned long)s->steps, s->ax, s->ay, s->az);
            if ((i & 0x3F) == 0) vTaskDelay(1);
        }
        printf("FLIGHT end\n");
    }
}

static void flight_recorder_start(void)
{
    flight_ring = heap_caps_malloc(FLIGHT_CAPACITY * sizeof(flight_sample_t),
                                   MALLOC_CAP_SPIRAM);
    if (flight_ring == NULL) {
        printf("flight: psram alloc failed\n");
        return;
    }
    xTaskCreate(flight_sample_task, "flight", 3072, NULL, 3, NULL);
    xTaskCreate(flight_console_task, "flightcon", 3072, NULL, 2, NULL);
    xTaskCreate(step_detect_task, "stepdet", 3072, NULL, 4, NULL);
}

int step_source_status(void)
{
    return status_code;
}

uint32_t step_source_total(void)
{
    static bool init_attempted;
    if (!init_attempted) {
        init_attempted = true;
        ready = pedometer_init();
        if (ready) flight_recorder_start();
    }
    if (!ready) return 0;


    /* Temporary diagnostics: prove the accel is sampling and the pedometer
       engine is enabled. Remove once step counting is confirmed. */
    static uint32_t last_dump_tick;
    if (!device_debug_quiet() && lv_tick_elaps(last_dump_tick) > 2000) {
        last_dump_tick = lv_tick_get();
        uint8_t hw_count[3] = {0};
        read_registers(REG_STEP_CNT_LOW, hw_count, 3);
        printf("qmi-dbg sw_steps=%lu hw_steps=%lu\n",
               (unsigned long)sw_steps,
               (unsigned long)(((uint32_t)hw_count[2] << 16) |
                               ((uint32_t)hw_count[1] << 8) | hw_count[0]));
    }
    return sw_steps;
}
