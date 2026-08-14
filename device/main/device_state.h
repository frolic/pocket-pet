#pragma once

/*
 * Device mode state machine: single owner of the power profile.
 * ACTIVE = screen on, full clocks. DOZING = dark; clocks relax on battery
 * and the manual light-sleep loop may engage.
 */

typedef enum {
    DEVICE_STATE_ACTIVE,
    DEVICE_STATE_DOZING,
} device_state_t;

void device_state_init(void);

device_state_t device_state_get(void);

/* Display-sleep transitions; must be called from LVGL context. */
void device_state_report_display(bool asleep);
