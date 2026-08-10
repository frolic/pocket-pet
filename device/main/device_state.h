#pragma once

#include <stdbool.h>

/*
 * Device mode state machine: single owner of the radio/display truce and the
 * power profile. See device_state.c for the state table.
 */

typedef enum {
    DEVICE_STATE_ACTIVE,
    DEVICE_STATE_DOZING,
    DEVICE_STATE_SYNCING,
    DEVICE_STATE_SYNC_VISIBLE,
    DEVICE_STATE_PORTAL,
} device_state_t;

void device_state_init(void);

device_state_t device_state_get(void);

/* Display-sleep transitions; must be called from LVGL context. */
void device_state_report_display(bool asleep);

/* Radio window gate: granted only while DOZING. */
bool device_state_request_radio(void);

void device_state_release_radio(void);

/* Boot-time STA sync started (screen is up: banner + frozen pet). */
void device_state_boot_sync(void);

/* Captive portal mode (setup modal; renderer gated by the flush gate). */
void device_state_portal(void);

/* Portal cancelled: back to normal operation. */
void device_state_portal_exit(void);
