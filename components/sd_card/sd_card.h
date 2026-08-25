#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bring up the on-board microSD card (SDMMC slot 0, hardwired pins) using the
 * ESP-IDF SDMMC host + FATFS, enable card power on GPIO45, then run a quick
 * self-test (write + read back a small file).
 *
 * Returns ESP_OK if the card mounted and the self-test passed. Returns an
 * error code if no card is present, power failed, or the FAT mount/write
 * failed. Call once at startup.
 *
 * This is a hardware validation layer only — it does not become part of the
 * filesystem layer the desktop will use (kept isolated so it can be replaced).
 */
esp_err_t arpile_sd_init(void);

/**
 * Optional callback, invoked with a line of human-readable text summarizing
 * the SD bring-up result (e.g. "SD ok: 15.9GB", "SD power fail",
 * "SD no card"). Runs during arpile_sd_init. The line is only valid for the
 * duration of the call.
 */
void arpile_sd_set_text_cb(void (*cb)(const char *text));

#ifdef __cplusplus
}
#endif