/* Shared hardware JPEG decoder helpers (ESP32-P4 JPEG engine).
 *
 * Decode-only, no software fallback: unsupported/oversized inputs return
 * NULL. Output is malloc'd RGB565 (BGR element order, BT.601), downscaled
 * with a nearest-neighbor pass to fit within max_w x max_h.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decode a JPEG buffer to RGB565 via the hardware decoder, scaling to fit
 * within max_w x max_h (aspect preserved). Input larger than hard_cap_w x
 * hard_cap_h is rejected (RAM guard for the raw decode surface).
 * Returns malloc'd pixels or NULL; caller frees.
 */
uint16_t *arpile_jpeg_decode_hw(const uint8_t *jpg, size_t jpg_len,
                                int max_w, int max_h,
                                int hard_cap_w, int hard_cap_h,
                                int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif
