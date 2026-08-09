#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "lvgl.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "step_source.h"

/*
 * QMI8658 hardware pedometer. Register sequences ported from lewisxhe's
 * SensorLib (MIT) QMI8658 pedometer support; tuning values from its example:
 * 2G range @ 62.5Hz, 200mg peak-to-peak, 100mg peak, 10-step entry filter.
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

/* CTRL9 command protocol: write command, wait for done bit, acknowledge. */
static bool run_command(uint8_t command)
{
    if (write_register(REG_CTRL9, command) != ESP_OK) return false;
    for (int i = 0; i < 200; i++) {
        uint8_t status = 0;
        if (read_registers(REG_STATUS_INT, &status, 1) == ESP_OK && (status & 0x80)) {
            write_register(REG_CTRL9, COMMAND_ACK);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    printf("qmi8658: command 0x%02x timed out\n", command);
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
        return false;
    }

    write_register(REG_RESET, 0xB0);
    vTaskDelay(pdMS_TO_TICKS(20));

    write_register(REG_CTRL1, 0x40); /* register address auto-increment */
    write_register(REG_CTRL2, 0x07); /* accel 2G range, 62.5Hz ODR */

    /* Pedometer configuration, two CAL-register batches (SensorLib recipe). */
    write_register(REG_CAL1_L, 50);   /* sample window */
    write_register(REG_CAL1_H, 0);
    write_register(REG_CAL2_L, 200);  /* peak-to-peak threshold, mg */
    write_register(REG_CAL2_H, 0);
    write_register(REG_CAL3_L, 100);  /* peak threshold, mg */
    write_register(REG_CAL3_H, 0);
    write_register(REG_CAL4_H, 0x01);
    write_register(REG_CAL4_L, 0x02);
    if (!run_command(COMMAND_CONFIGURE_PEDOMETER)) return false;

    write_register(REG_CAL1_L, 200);  /* step timeout window */
    write_register(REG_CAL1_H, 0);
    write_register(REG_CAL2_L, 20);   /* step quiet time */
    write_register(REG_CAL2_H, 10);   /* entry count — filters fake steps */
    write_register(REG_CAL3_L, 0);    /* precision */
    write_register(REG_CAL3_H, 4);    /* update registers every 4 steps */
    write_register(REG_CAL4_H, 0x02);
    write_register(REG_CAL4_L, 0x02);
    if (!run_command(COMMAND_CONFIGURE_PEDOMETER)) return false;

    write_register(REG_CTRL7, 0x01);  /* accelerometer on */
    uint8_t ctrl8 = 0;
    read_registers(REG_CTRL8, &ctrl8, 1);
    write_register(REG_CTRL8, ctrl8 | (1 << 4)); /* pedometer on */

    printf("qmi8658: pedometer running\n");
    return true;
}

uint32_t step_source_total(void)
{
    static bool init_attempted;
    if (!init_attempted) {
        init_attempted = true;
        ready = pedometer_init();
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
    return cached_steps;
}
