#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

/*
 * Builds the full watchface (clock, pet, step count, goal ring) on parent —
 * the 410x502 panel: the screen on device, a panel container in the sim.
 */
void watchface_create(lv_obj_t *parent);

/* Updates the step count label and the daily-goal ring. */
void watchface_set_steps(uint32_t total);

/* Shows/hides the recording indicator while the record button is held. */
void watchface_set_recording(bool recording);

/* Shows a centered status banner (e.g. "WIFI SETUP"), or hides it with NULL. */
void watchface_set_banner(const char *text);

/* Blacks out the scene beneath the banner (boot clock sync reads as a boot
   screen, not a frozen game). */
void watchface_set_boot_cover(bool shown);

/* Battery indicator (top right). percent < 0 hides it. */
void watchface_set_battery(int percent, bool charging);

/* Wifi-disconnected icon beside the battery (hidden when online). */
void watchface_set_wifi_offline(bool offline);

/* Tapping the wifi-disconnected icon (device: reboot into setup). */
void watchface_set_wifi_tap_cb(void (*tap_cb)(void));

/* Full setup-mode modal ("WIFI SETUP" + cancel). The cancel hit region is
   the modal's lower band; device code hit-tests raw touch against
   watchface_setup_modal_cancel_min_y(). */
void watchface_show_setup_modal(bool show);

int watchface_setup_modal_cancel_min_y(void);
