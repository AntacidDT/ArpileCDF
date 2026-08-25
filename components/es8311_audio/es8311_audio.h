#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the onboard ES8311 audio codec (Waveshare ESP32-P4-NANO).
 * Sets up I2C (codec control), I2S std TX, enables the PA, and opens the
 * codec at 16 kHz / 16-bit / stereo. Returns ESP_OK on success.
 */
esp_err_t arpile_audio_init(void);

/**
 * Play a sine test tone of the given frequency for the given duration.
 * Must be called after arpile_audio_init().
 */
esp_err_t arpile_audio_play_tone(uint16_t freq_hz, uint32_t duration_ms);

/** True if the codec is initialized and ready to accept PCM. */
bool arpile_audio_ready(void);

/**
 * Write interleaved stereo 16-bit PCM at the configured sample rate
 * (16 kHz). Blocks until all samples are queued. Must be called after
 * arpile_audio_init().
 */
esp_err_t arpile_audio_write_pcm(const int16_t *stereo_interleaved,
                                 size_t sample_pairs);

/** Set output volume 0-100 (clamped). */
void arpile_audio_set_volume(int vol);

/** Get current output volume (0-100). */
int arpile_audio_get_volume(void);

/** Release the codec and I2S resources. */
void arpile_audio_deinit(void);

#ifdef __cplusplus
}
#endif
