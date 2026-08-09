#include <stdio.h>
#include "driver/i2c_master.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "power_button.h"

/*
 * AXP2101 PMIC power-key short press, polled via IRQ status. Long press
 * remains the PMIC's hardware power-off. Register facts from XPowersLib:
 * INTSTS2 (0x49) bit 3 = short press, write-1-to-clear; enabled in
 * INTEN2 (0x41).
 */

#define AXP2101_ADDRESS 0x34
#define REG_INTEN2 0x41
#define REG_INTSTS2 0x49
#define PKEY_SHORT_BIT 0x08

static i2c_master_dev_handle_t device;
static bool ready;

static bool init(void)
{
    bsp_i2c_init();
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDRESS,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &device) != ESP_OK) {
        printf("axp2101: not reachable — power button disabled\n");
        return false;
    }
    /* Enable the short-press IRQ and clear any stale status. */
    uint8_t reg = REG_INTEN2;
    uint8_t enable = 0;
    i2c_master_transmit_receive(device, &reg, 1, &enable, 1, 100);
    uint8_t enable_write[2] = {REG_INTEN2, (uint8_t)(enable | PKEY_SHORT_BIT)};
    i2c_master_transmit(device, enable_write, 2, 100);
    uint8_t clear[2] = {REG_INTSTS2, PKEY_SHORT_BIT};
    i2c_master_transmit(device, clear, 2, 100);
    printf("axp2101: power button ready\n");
    return true;
}

bool power_button_pressed(void)
{
    static bool init_attempted;
    if (!init_attempted) {
        init_attempted = true;
        ready = init();
    }
    if (!ready) return false;

    uint8_t reg = REG_INTSTS2;
    uint8_t status = 0;
    if (i2c_master_transmit_receive(device, &reg, 1, &status, 1, 100) != ESP_OK) {
        return false;
    }
    if (status & PKEY_SHORT_BIT) {
        uint8_t clear[2] = {REG_INTSTS2, PKEY_SHORT_BIT};
        i2c_master_transmit(device, clear, 2, 100);
        return true;
    }
    return false;
}
