#include <stdio.h>
#include "driver/i2c_master.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "power_button.h"
#include "battery_source.h"
#include "device_axp2101.h"

/*
 * AXP2101 PMIC: power-key short press (polled via IRQ status; long press
 * remains the PMIC's hardware power-off) and the battery fuel gauge.
 * Register facts from XPowersLib: INTSTS2 (0x49) bit 3 = short press,
 * write-1-to-clear, enabled in INTEN2 (0x41); STATUS2 (0x01) bits 6:5 = 01
 * while charging; BAT_PERCENT (0xA4) = gauge output 0-100.
 */

#define AXP2101_ADDRESS 0x34
#define REG_STATUS1 0x00
#define REG_STATUS2 0x01
#define REG_INTEN2 0x41
#define REG_INTSTS2 0x49
#define REG_BAT_PERCENT 0xA4
#define PKEY_SHORT_BIT 0x08
#define PKEY_LONG_BIT 0x04

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
    uint8_t enable_write[2] = {REG_INTEN2,
                               (uint8_t)(enable | PKEY_SHORT_BIT | PKEY_LONG_BIT)};
    i2c_master_transmit(device, enable_write, 2, 100);
    uint8_t clear[2] = {REG_INTSTS2, PKEY_SHORT_BIT | PKEY_LONG_BIT};
    i2c_master_transmit(device, clear, 2, 100);
    printf("axp2101: power button ready\n");
    return true;
}

static bool ensure_ready(void)
{
    static bool init_attempted;
    if (!init_attempted) {
        init_attempted = true;
        ready = init();
    }
    return ready;
}

static bool read_register(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(device, &reg, 1, value, 1, 100) == ESP_OK;
}

int battery_source_percent(void)
{
    uint8_t percent;
    if (!ensure_ready() || !read_register(REG_BAT_PERCENT, &percent)) return -1;
    if (percent > 100) return -1;
    return percent;
}

bool battery_source_charging(void)
{
    uint8_t status;
    if (!ensure_ready() || !read_register(REG_STATUS2, &status)) return false;
    return ((status >> 5) & 0x03) == 0x01;
}

bool axp2101_vbus_present(void)
{
    uint8_t status;
    /* Fail toward "plugged in": callers gate power saving — including the
       manual light-sleep loop — on VBUS absence, so an I2C hiccup must read
       as "still powered", never as a license to sleep. */
    if (!ensure_ready() || !read_register(REG_STATUS1, &status)) return true;
    return (status & (1 << 5)) != 0; /* VBUS good */
}

bool power_button_pressed(void)
{
    if (!ensure_ready()) return false;

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

bool power_button_long_pressed(void)
{
    if (!ensure_ready()) return false;
    uint8_t reg = REG_INTSTS2;
    uint8_t status = 0;
    if (i2c_master_transmit_receive(device, &reg, 1, &status, 1, 100) != ESP_OK) {
        return false;
    }
    if (status & PKEY_LONG_BIT) {
        uint8_t clear[2] = {REG_INTSTS2, PKEY_LONG_BIT};
        i2c_master_transmit(device, clear, 2, 100);
        return true;
    }
    return false;
}
