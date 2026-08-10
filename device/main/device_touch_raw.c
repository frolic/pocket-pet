#include <stdbool.h>
#include "driver/i2c_master.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "device_touch_raw.h"

/*
 * Raw FT3168 touch read (FT5x06-class registers), for states where LVGL's
 * renderer — and therefore its input pipeline — is deliberately stopped.
 */

#define FT3168_ADDRESS 0x38
#define REG_TD_STATUS 0x02

static i2c_master_dev_handle_t device;
static bool ready;

static bool ensure_ready(void)
{
    static bool attempted;
    if (attempted) return ready;
    attempted = true;
    bsp_i2c_init();
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = FT3168_ADDRESS,
        .scl_speed_hz = 400000,
    };
    ready = i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &device) == ESP_OK;
    return ready;
}

bool device_touch_raw_get(int *x, int *y)
{
    if (!ensure_ready()) return false;
    uint8_t reg = REG_TD_STATUS;
    uint8_t buffer[5];
    if (i2c_master_transmit_receive(device, &reg, 1, buffer, 5, 50) != ESP_OK) return false;
    if ((buffer[0] & 0x0F) == 0) return false;
    *x = ((buffer[1] & 0x0F) << 8) | buffer[2];
    *y = ((buffer[3] & 0x0F) << 8) | buffer[4];
    return true;
}
