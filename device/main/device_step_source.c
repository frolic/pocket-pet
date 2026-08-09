#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "lvgl.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "step_source.h"

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
static uint32_t cached_steps;
static uint32_t cached_at_tick;

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

    /* The counter registers update every 4 steps; polling faster than 500ms
       just burns i2c traffic. */
    if (cached_at_tick != 0 && lv_tick_elaps(cached_at_tick) < 500) {
        return cached_steps;
    }
    uint8_t buffer[3];
    if (read_registers(REG_STEP_CNT_LOW, buffer, 3) == ESP_OK) {
        cached_steps = ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[1] << 8) | buffer[0];
        cached_at_tick = lv_tick_get();
    }

    /* Temporary diagnostics: prove the accel is sampling and the pedometer
       engine is enabled. Remove once step counting is confirmed. */
    static uint32_t last_dump_tick;
    if (lv_tick_elaps(last_dump_tick) > 2000) {
        last_dump_tick = lv_tick_get();
        uint8_t ctrl[8] = {0};
        uint8_t status0 = 0;
        uint8_t accel[6] = {0};
        read_registers(REG_CTRL1, ctrl, 8);      /* CTRL1..CTRL8 = 0x02..0x09 */
        read_registers(0x2E, &status0, 1);       /* STATUS0: data-ready bits */
        read_registers(0x35, accel, 6);          /* AX_L..AZ_H */
        int16_t ax = (int16_t)(accel[0] | (accel[1] << 8));
        int16_t ay = (int16_t)(accel[2] | (accel[3] << 8));
        int16_t az = (int16_t)(accel[4] | (accel[5] << 8));
        printf("qmi-dbg ctrl2=%02x ctrl7=%02x ctrl8=%02x status0=%02x "
               "accel=%d,%d,%d steps=%lu\n",
               ctrl[1], ctrl[6], ctrl[7], status0,
               ax, ay, az, (unsigned long)cached_steps);
    }
    return cached_steps;
}
