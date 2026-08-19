#pragma once

#include <stdbool.h>
#include "lvgl.h"

/*
 * Screen sleep: after a period without input the display fades out (panel
 * brightness ramps down while a top-layer overlay cross-fades through gray
 * to black and swallows touches) and pet animation freezes. Steps keep
 * counting — the IMU runs independently. Waking is explicit (the side
 * button) via display_sleep_wake().
 */

void display_sleep_init(uint32_t timeout_ms);

/* Registers the platform brightness hook (percent, 0 = pixels off). */
void display_sleep_set_dim_cb(void (*dim_cb)(uint8_t brightness_percent));

/* Temporary brightness boost (PWR double-tap); reverts when the screen
   next sleeps. Applies immediately if lit. */
void display_sleep_boost(uint8_t percent);

/* Fires when the screen finishes going dark (fade complete) or wakes.
   Called from LVGL context. */
void display_sleep_set_state_cb(void (*state_cb)(bool asleep));

bool display_sleep_is_asleep(void);

/* Any user activity: postpones (or does nothing if already asleep). */
void display_sleep_poke(void);

/* Wakes the display (power button). */
void display_sleep_wake(void);

/* Sleeps immediately (power button toggle). */
void display_sleep_sleep_now(void);

/* While held, the inactivity timeout is suspended (setup portal). */
void display_sleep_set_hold(bool hold);
