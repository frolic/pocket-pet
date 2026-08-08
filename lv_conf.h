/*
 * Minimal LVGL config: anything not set here falls back to LVGL defaults
 * via lv_conf_internal.h. Keep this file small — only real decisions.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* RGB565, matching the CO5300 panel on the target device. */
#define LV_COLOR_DEPTH 16

/* Simulator backend. The device build replaces this with the CO5300/QSPI driver. */
#define LV_USE_SDL 1
/* Homebrew's SDL2 include dir already points inside SDL2/. */
#define LV_SDL_INCLUDE_PATH <SDL.h>

/* Fonts used by the watchface (default 14pt is also enabled). */
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_44 1

#endif /* LV_CONF_H */
