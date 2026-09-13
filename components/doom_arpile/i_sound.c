/* Sound-effects adapter: mixes DOOM's 11 kHz u8 PC-speaker samples and feeds
 * the existing Arpile ES8311 PCM API (16 kHz stereo s16). Music is stubbed
 * (no synth in this MVP) — reported separately. */
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include "config.h"
#include "z_zone.h"
#include "m_swap.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"
#include "lprintf.h"
#include "s_sound.h"

#include "doomdef.h"
#include "doomstat.h"
#include "doomtype.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

#include "es8311_audio.h"

#define VOICES      8
#define OUT_RATE    16000
#define CHUNK       256          /* stereo sample pairs per write */

typedef struct {
    const unsigned char *data;    /* points past the 6-byte header */
    int len;
    int pos;
    int step;                     /* 16.16 resample step (11025<<16)/rate_out */
    unsigned acc;
    int vol;                      /* 0..64 */
} voice_t;

static voice_t s_voices[VOICES];
static SemaphoreHandle_t s_mux;
static volatile bool s_audio_on;

int snd_card = 1, mus_card = 0;
int snd_samplerate = OUT_RATE;

void I_UpdateSoundParams(int handle, int volume, int seperation, int pitch)
{
    if (!s_mux || handle < 0 || handle >= VOICES) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_voices[handle].data) {
        int v = volume * 64 / 128;
        s_voices[handle].vol = v > 64 ? 64 : v;
        /* pitch: engine uses pitch<<16 as speed factor */
        s_voices[handle].step = (pitch > 0)
            ? (int)(((long long)((11025 << 16) / OUT_RATE)) * 256 / (pitch >> 8 ? (pitch >> 8) : 1))
            : (11025 << 16) / OUT_RATE;
        if (s_voices[handle].step <= 0) {
            s_voices[handle].step = (11025 << 16) / OUT_RATE;
        }
    }
    xSemaphoreGive(s_mux);
}

void I_SetChannels(void) {}

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char buf[9];
    snprintf(buf, sizeof(buf), "ds%s", sfx->name);
    for (char *c = buf; *c; c++) {
        *c = toupper((unsigned char)*c);
    }
    return W_CheckNumForName(buf);
}

int I_StartSound(int id, int channel, int vol, int sep, int pitch, int priority)
{
    if (!s_mux || id < 0 || id >= NUMSFX) {
        return -1;
    }
    int lump = I_GetSfxLumpNum(&S_sfx[id]);
    if (lump < 0) {
        return -1;
    }
    const unsigned char *lumpdata = (const unsigned char *)W_CacheLumpNum(lump);
    if (!lumpdata || lumpdata[0] != 3) {   /* DOOM digi magic */
        return -1;
    }
    int rate = lumpdata[2] | (lumpdata[3] << 8);
    int len  = lumpdata[4] | (lumpdata[5] << 8);
    if (rate <= 0 || len <= 0) {
        return -1;
    }

    xSemaphoreTake(s_mux, portMAX_DELAY);
    int slot = channel >= 0 && channel < VOICES ? channel : 0;
    for (int i = 0; i < VOICES && s_voices[slot].data; i++) {
        slot = i;                          /* steal first free/oldest */
        if (!s_voices[i].data) { break; }
    }
    voice_t *v = &s_voices[slot];
    v->data = lumpdata + 6;
    v->len = len;
    v->pos = 0;
    v->acc = 0;
    v->step = (int)(((unsigned long long)(rate) << 16) / OUT_RATE);
    v->vol = vol * 64 / 128;
    if (v->vol > 64) { v->vol = 64; }
    xSemaphoreGive(s_mux);
    return slot;
}

void I_StopSound(int handle)
{
    if (!s_mux || handle < 0 || handle >= VOICES) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_voices[handle].data = NULL;
    xSemaphoreGive(s_mux);
}

int I_SoundIsPlaying(int handle)
{
    if (!s_mux || handle < 0 || handle >= VOICES) {
        return 0;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int playing = s_voices[handle].data != NULL;
    xSemaphoreGive(s_mux);
    return playing;
}

int I_AnySoundStillPlaying(void)
{
    if (!s_mux) {
        return false;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int any = 0;
    for (int i = 0; i < VOICES; i++) {
        if (s_voices[i].data) { any = 1; break; }
    }
    xSemaphoreGive(s_mux);
    return any;
}

void I_ShutdownSound(void)
{
    s_audio_on = false;
}

/* Mixer: runs until doom_engine_stop tears audio down. */
static void doom_audio_task(void *arg)
{
    static int16_t buf[CHUNK * 2];
    while (s_audio_on) {
        if (!arpile_audio_ready()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        xSemaphoreTake(s_mux, portMAX_DELAY);
        for (int i = 0; i < CHUNK; i++) {
            int acc = 0;
            for (int vI = 0; vI < VOICES; vI++) {
                voice_t *v = &s_voices[vI];
                if (!v->data) { continue; }
                acc += (v->data[v->pos] - 128) * v->vol >> 6;
                v->acc += (unsigned)v->step;
                while (v->acc >= 0x10000u) {
                    v->acc -= 0x10000u;
                    v->pos++;
                    if (v->pos >= v->len) {
                        v->data = NULL;
                        v->pos = 0;
                        break;
                    }
                }
            }
            if (acc > 127)  { acc = 127; }
            if (acc < -128) { acc = -128; }
            /* ~40% master volume so SFX aren't deafening */
            int16_t s = (int16_t)((acc * 5 / 12) << 8);
            buf[i * 2] = s;
            buf[i * 2 + 1] = s;
        }
        xSemaphoreGive(s_mux);
        arpile_audio_write_pcm(buf, CHUNK);
    }
    vTaskDelete(NULL);
}

void I_InitSound(void)
{
    if (!s_mux) {
        s_mux = xSemaphoreCreateMutex();
    }
    memset(s_voices, 0, sizeof(s_voices));
    for (int i = 0; i < VOICES; i++) {
        s_voices[i].step = (11025 << 16) / OUT_RATE;
    }
    if (!s_audio_on) {
        s_audio_on = true;
        xTaskCreatePinnedToCore(doom_audio_task, "doomaudio", 4096,
                                NULL, 6, NULL, 0);
    }
}

void I_ShutdownMusic(void) {}
void I_InitMusic(void) {}
void I_PlaySong(int handle, int looping) {}

extern int mus_pause_opt;

void I_PauseSong(int handle) {}
void I_ResumeSong(int handle) {}
void I_StopSong(int handle) {}
void I_UnRegisterSong(int handle) {}

int I_RegisterSong(const void *data, size_t len) { return 0; }

int I_RegisterMusic(const char *filename, musicinfo_t *song) { return 1; }

void I_SetMusicVolume(int volume) {}
