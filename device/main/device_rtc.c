#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "driver/i2c_master.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "device_rtc.h"

/*
 * PCF85063 RTC. Time lives in BCD registers 0x04-0x0A (sec, min, hour,
 * day, weekday, month, year-since-2000); bit 7 of the seconds register is
 * the oscillator-stop (OS) flag — set on power loss, meaning the time is
 * not to be trusted. Writing the time clears it. The chip stays powered
 * whenever the battery is in, so in practice only a battery pull (or first
 * boot ever) invalidates it. Stored time is UTC; TZ conversion is the
 * system's job.
 */

#define PCF85063_ADDRESS 0x51
#define REG_SECONDS 0x04
#define OS_FLAG 0x80

static i2c_master_dev_handle_t device;
static bool ready;
static bool time_valid;

static bool ensure_ready(void)
{
    static bool init_attempted;
    if (init_attempted) return ready;
    init_attempted = true;
    bsp_i2c_init();
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_ADDRESS,
        .scl_speed_hz = 400000,
    };
    ready = i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &device) == ESP_OK;
    if (!ready) printf("device_rtc: PCF85063 not reachable\n");
    return ready;
}

static uint8_t from_bcd(uint8_t value)
{
    return (value >> 4) * 10 + (value & 0x0F);
}

static uint8_t to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

bool device_rtc_restore(void)
{
    if (!ensure_ready()) return false;
    uint8_t reg = REG_SECONDS;
    uint8_t raw[7];
    if (i2c_master_transmit_receive(device, &reg, 1, raw, sizeof(raw), 100) != ESP_OK) {
        return false;
    }
    if (raw[0] & OS_FLAG) {
        printf("device_rtc: oscillator-stop set — time invalid\n");
        return false;
    }
    struct tm utc = {
        .tm_sec = from_bcd(raw[0] & 0x7F),
        .tm_min = from_bcd(raw[1] & 0x7F),
        .tm_hour = from_bcd(raw[2] & 0x3F),
        .tm_mday = from_bcd(raw[3] & 0x3F),
        .tm_mon = from_bcd(raw[5] & 0x1F) - 1,
        .tm_year = from_bcd(raw[6]) + 100, /* chip year 00-99 = 2000-2099 */
    };
    if (utc.tm_year < 126 || utc.tm_mday == 0) { /* earlier than 2026: nonsense */
        printf("device_rtc: implausible date — ignoring\n");
        return false;
    }
    /* newlib has no timegm; civil-days conversion (Hinnant) avoids TZ. */
    int year = utc.tm_year + 1900;
    int month = utc.tm_mon + 1;
    int era_year = month <= 2 ? year - 1 : year;
    int64_t era = era_year / 400;
    int yoe = era_year - era * 400;
    int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + utc.tm_mday - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    time_t epoch = days * 86400 + utc.tm_hour * 3600 + utc.tm_min * 60 + utc.tm_sec;
    if (epoch <= 0) return false;
    struct timeval now = {.tv_sec = epoch};
    settimeofday(&now, NULL);
    printf("device_rtc: clock restored (%04d-%02d-%02d %02d:%02d UTC)\n",
           utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
           utc.tm_min);
    time_valid = true;
    return true;
}

bool device_rtc_time_valid(void)
{
    return time_valid;
}

bool device_rtc_store(void)
{
    if (!ensure_ready()) return false;
    time_t epoch = time(NULL);
    struct tm utc;
    gmtime_r(&epoch, &utc);
    uint8_t frame[8] = {
        REG_SECONDS,
        to_bcd((uint8_t)utc.tm_sec), /* OS flag bit written 0 = time valid */
        to_bcd((uint8_t)utc.tm_min),
        to_bcd((uint8_t)utc.tm_hour),
        to_bcd((uint8_t)utc.tm_mday),
        to_bcd((uint8_t)utc.tm_wday),
        to_bcd((uint8_t)(utc.tm_mon + 1)),
        to_bcd((uint8_t)(utc.tm_year - 100)),
    };
    if (i2c_master_transmit(device, frame, sizeof(frame), 100) != ESP_OK) {
        printf("device_rtc: store failed\n");
        return false;
    }
    printf("device_rtc: clock stored\n");
    return true;
}
