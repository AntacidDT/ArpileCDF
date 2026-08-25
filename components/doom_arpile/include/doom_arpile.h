/* DOOM → Arpile compatibility layer public API. */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "ili9488.h"
#include "arpile_ui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the DOOM engine task. Loads /doomstore/DOOM1.WAD from the flash
 * storage partition first. Returns ESP_ERR_NOT_FOUND if the WAD is missing. */
esp_err_t doom_engine_start(void);

/* Request engine shutdown; returns after the engine task has exited. */
void doom_engine_stop(void);

/* True while the engine task is running. */
bool doom_engine_running(void);

/* Blit the most recent engine frame (320x240 palette8, converted to RGB565)
 * scaled into `dst`. Called from the UI task only. */
void doom_video_blit(ili9488_t *lcd, const ui_rect_t *dst);

/* Feed one key event into the engine (HID-task safe).
 * down=true for press, false for release. */
void doom_input_post(bool down, int doom_key);

#ifdef __cplusplus
}
#endif
