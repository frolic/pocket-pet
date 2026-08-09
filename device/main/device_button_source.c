#include "driver/gpio.h"
#include "button_source.h"

/* The side BOOT button: plain GPIO0, active low. */
#define RECORD_BUTTON GPIO_NUM_0

bool button_source_held(void)
{
    static bool initialized;
    if (!initialized) {
        gpio_config_t config = {
            .pin_bit_mask = 1ULL << RECORD_BUTTON,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&config);
        initialized = true;
    }
    return gpio_get_level(RECORD_BUTTON) == 0;
}
