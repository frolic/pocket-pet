#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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
#include "device_step_source.h"
#include "device_debug.h"

/*
 * QMI8658 accelerometer as a step source. Register sequences ported from
 * lewisxhe's SensorLib (MIT).
 *
 * The accel samples continuously at 31.25Hz into the chip's on-die FIFO
 * (128 samples ~= 4s of waveform); the host drains it in one I2C burst
 * about once a second and runs the software step detector over the batch
 * with per-sample timestamps. Sampling is metronomic on the IMU's clock
 * (no FreeRTOS jitter), nothing is lost while the host light-sleeps, and
 * the dark loop only has to wake ~1x/s instead of 25x/s. Counts land at
 * most ~1s late — accuracy over realtime, by design.
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
#define REG_FIFO_WTM_TH 0x13
#define REG_FIFO_CTRL 0x14
#define REG_FIFO_COUNT 0x15
#define REG_FIFO_STATUS 0x16
#define REG_FIFO_DATA 0x17
#define REG_STATUS_INT 0x2D
#define REG_STEP_CNT_LOW 0x5A
#define REG_RESET 0x60

#define WHOAMI_VALUE 0x05
#define COMMAND_CONFIGURE_PEDOMETER 0x0D
#define COMMAND_REQ_FIFO 0x05
#define COMMAND_ACK 0x00

/* 128-sample stream-mode FIFO (bits[3:2]=3, bits[1:0]=2): oldest samples
   overwrite when full, so a late drain degrades gracefully. */
#define FIFO_CTRL_CONFIG 0x0E
#define FIFO_SAMPLE_MS 32           /* 31.25Hz accel ODR */
#define FIFO_MAX_SAMPLES 128

static i2c_master_dev_handle_t device;
static bool ready;
/* All QMI access serialized: the drain's CTRL9 REQ_FIFO sequence is
   multi-transaction, and a foreign register read interleaved mid-read-mode
   jams the FIFO engine (the 2026-08-16 wedge — introduced by the debug
   heartbeat's own fifo_words read racing the drain). */
static SemaphoreHandle_t qmi_mutex;
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

static bool configure_sensor(void);

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
    qmi_mutex = xSemaphoreCreateMutex();
    return configure_sensor();
}

static bool configure_sensor(void)
{
    write_register(REG_RESET, 0xB0);
    vTaskDelay(pdMS_TO_TICKS(20));

    write_register(REG_CTRL1, 0x40); /* register address auto-increment */
    write_register(REG_CTRL8, 0x80); /* CTRL9 handshake via STATUS_INT.7 */
    write_register(REG_CTRL2, 0x18); /* accel 4G range, 31.25Hz ODR */

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
    write_register(REG_CTRL8, ctrl8 | (1 << 4)); /* pedometer on (diag only) */
    write_register(REG_FIFO_WTM_TH, 0);
    write_register(REG_FIFO_CTRL, FIFO_CTRL_CONFIG);

    printf("qmi8658: accel sampling into fifo\n");
    return true;
}

/*
 * Software step detector. The QMI8658's on-die pedometer engine never
 * produced a count on this board even with verified-executed configuration
 * (CTRL9 handshake confirmed, reference thresholds), so steps are detected
 * here instead: accel magnitude at the FIFO's 31.25Hz, EMA gravity
 * baseline, positive-peak detection gated to walking cadence, and an entry
 * filter so lone bumps don't count. The hardware counter is still read in
 * diagnostics should a future silicon revision start working.
 */

/* 4G range: 8192 counts per g. */
#define SW_PEAK_THRESHOLD 900.0f    /* ~0.11g above baseline */
#define SW_MIN_STEP_MS 280          /* max cadence ~3.5 steps/s */
#define SW_MAX_STEP_MS 1000         /* min cadence 1 step/s */
#define SW_ENTRY_STEPS 4
#define SW_RESET_GAP_MS 1400

static volatile uint32_t sw_steps;
static volatile float batch_min_mag, batch_max_mag; /* last drain, diagnostics */
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

/* Detector state persists across pacing modes: the dark-time sleep loop
   takes over sampling mid-walk without losing the cadence run. */
static float detect_baseline = 8192.0f;
static bool detect_above;
static uint32_t detect_last_peak_ms;
static int detect_entry_run;
static uint32_t persist_last_ms;
static volatile bool external_pacing;

void step_source_external_pacing(bool external)
{
    external_pacing = external;
}

/* One accel sample through the peak detector. `now` is the sample's own
   timestamp (reconstructed from FIFO position), not the processing time —
   cadence gating stays exact however late the batch is drained. */
static void detect_process(float magnitude, uint32_t now)
{
    /* EMA gravity baseline; 0.04 at 32ms/sample matches the time constant
       the thresholds were tuned against (0.05 at 40ms). */
    detect_baseline += 0.04f * (magnitude - detect_baseline);
    float deviation = magnitude - detect_baseline;

    if (detect_entry_run > 0 && now - detect_last_peak_ms > SW_RESET_GAP_MS) {
        detect_entry_run = 0;
    }
    if (!detect_above && deviation > SW_PEAK_THRESHOLD) {
        detect_above = true;
        uint32_t gap = now - detect_last_peak_ms;
        if (gap >= SW_MIN_STEP_MS && gap <= SW_MAX_STEP_MS) {
            if (detect_entry_run < SW_ENTRY_STEPS) {
                detect_entry_run++;
                if (detect_entry_run == SW_ENTRY_STEPS) {
                    sw_steps += SW_ENTRY_STEPS; /* credit the run-up */
                }
            } else {
                sw_steps++;
            }
        } else {
            detect_entry_run = 1; /* rhythm broken: this peak starts a new run */
        }
        detect_last_peak_ms = now;
    } else if (detect_above && deviation < SW_PEAK_THRESHOLD * 0.5f) {
        detect_above = false;
    }
}

void step_source_drain_now(void)
{
    if (!ready) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - persist_last_ms >= 30000) {
        persist_last_ms = now;
        steps_persist_tick();
    }
    if (xSemaphoreTake(qmi_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

    /* Word count lives in COUNT plus STATUS[1:0]; accel-only samples are
       three words each. */
    uint8_t count_regs[2];
    if (read_registers(REG_FIFO_COUNT, count_regs, 2) != ESP_OK) {
        xSemaphoreGive(qmi_mutex);
        return;
    }
    uint16_t words = ((uint16_t)(count_regs[1] & 0x03) << 8) | count_regs[0];
    uint16_t samples = words / 3;
    if (samples == 0) {
        /* A still watch still samples gravity — sustained empty means the
           FIFO wedged (a failed CTRL9 handshake can strand it in read
           mode, where capture stops). Re-arm it. */
        static int empty_run;
        if (++empty_run >= 5) {
            empty_run = 0;
            configure_sensor(); /* full reset + reconfig; re-arm alone
                                   does not clear a jammed read mode */
            printf("qmi8658: fifo re-initialized after empty run\n");
        }
        xSemaphoreGive(qmi_mutex);
        return;
    }
    if (samples > FIFO_MAX_SAMPLES) samples = FIFO_MAX_SAMPLES;

    static uint8_t batch[FIFO_MAX_SAMPLES * 6];
    if (!run_command(COMMAND_REQ_FIFO)) { /* enter FIFO read mode */
        xSemaphoreGive(qmi_mutex);
        return;
    }
    esp_err_t read_result = read_registers(REG_FIFO_DATA, batch, samples * 6);
    write_register(REG_FIFO_CTRL, FIFO_CTRL_CONFIG); /* exit read mode */
    if (read_result != ESP_OK) {
        xSemaphoreGive(qmi_mutex);
        return;
    }

    float lo = 1e9f, hi = 0;
    for (uint16_t i = 0; i < samples; i++) {
        const uint8_t *b = batch + i * 6;
        int16_t ax = (int16_t)(b[0] | (b[1] << 8));
        int16_t ay = (int16_t)(b[2] | (b[3] << 8));
        int16_t az = (int16_t)(b[4] | (b[5] << 8));
        float magnitude = sqrtf((float)ax * ax + (float)ay * ay + (float)az * az);
        if (magnitude < lo) lo = magnitude;
        if (magnitude > hi) hi = magnitude;
        detect_process(magnitude,
                       now - (uint32_t)(samples - 1 - i) * FIFO_SAMPLE_MS);
    }
    batch_min_mag = lo;
    batch_max_mag = hi;
    xSemaphoreGive(qmi_mutex);
}

static void step_detect_task(void *arg)
{
    (void)arg;
    steps_restore();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        /* While the light-sleep loop paces draining (device_sleep.c), the
           task stands down so the FIFO isn't drained twice per window. */
        if (!external_pacing) step_source_drain_now();
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
    /* Flight recorder retired (it found what it needed to find); its
       internal-RAM task stacks now fund the portal's httpd instead. */
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
        uint8_t fifo_regs[2] = {0};
        if (qmi_mutex != NULL && xSemaphoreTake(qmi_mutex, 0) == pdTRUE) {
            read_registers(REG_STEP_CNT_LOW, hw_count, 3);
            read_registers(REG_FIFO_COUNT, fifo_regs, 2);
            xSemaphoreGive(qmi_mutex);
        }
        printf("qmi-dbg sw_steps=%lu hw_steps=%lu fifo_words=%u base=%.0f\n",
               (unsigned long)sw_steps,
               (unsigned long)(((uint32_t)hw_count[2] << 16) |
                               ((uint32_t)hw_count[1] << 8) | hw_count[0]),
               (unsigned)(((fifo_regs[1] & 0x03) << 8) | fifo_regs[0]),
               (double)detect_baseline);
        printf("qmi-dbg batch min=%.0f max=%.0f\n",
               (double)batch_min_mag, (double)batch_max_mag);
    }
    return sw_steps;
}
