/* Video adapter: engine's 320x240 8-bit palette framebuffer → RGB565 buffer
 * → scaled blit through the existing ILI9488 pipeline. Replaces esp32-doom's
 * spi_lcd.c entirely; no second display driver exists here. */
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "doomstat.h"
#include "doomdef.h"
#include "doomtype.h"
#include "v_video.h"
#include "r_draw.h"
#include "d_main.h"
#include "i_video.h"
#include "z_zone.h"
#include "st_stuff.h"
#include "lprintf.h"
#include "w_wad.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"

#include "ili9488.h"
#include "arpile_ui_core.h"
#include "doom_arpile.h"

int use_fullscreen = 0;
int use_doublebuffer = 0;

static uint8_t  *s_pal_fb;      /* 320x240 palette8, written by engine */
static uint8_t  *s_666_fb;      /* 320x240 packed RGB666, DMA/streamed */
static volatile bool s_frame_ready;
static SemaphoreHandle_t s_fb_mux;

/* RGB666 byte-triplet lookup built by I_SetPalette from PLAYPAL */
static uint8_t s_lut666[256 * 3];

void I_StartTic(void)
{
    extern void doom_input_poll(void);
    doom_input_poll();                /* engine drains queued keys per tic */
}

static void I_InitInputs(void) {}

static void I_UploadNewPalette(int pal) {}

void I_ShutdownGraphics(void) {}

void I_UpdateNoBlit(void) {}

void I_StartFrame(void) {}

int I_StartDisplay(void) { return true; }

void I_EndDisplay(void) {}

/* Engine finished a frame: convert palette→RGB565 and mark ready. */
void I_FinishUpdate(void)
{
    if (!s_pal_fb || !s_666_fb || !s_fb_mux) {
        return;
    }
    xSemaphoreTake(s_fb_mux, portMAX_DELAY);
    const int n = SCREENWIDTH * SCREENHEIGHT;
    for (int i = 0; i < n; i++) {
        const uint8_t *lut = &s_lut666[s_pal_fb[i] * 3];
        s_666_fb[i * 3]     = lut[0];
        s_666_fb[i * 3 + 1] = lut[1];
        s_666_fb[i * 3 + 2] = lut[2];
    }
    xSemaphoreGive(s_fb_mux);
    s_frame_ready = true;
}

void I_SetPalette(int pal)
{
    int pplump = W_GetNumForName("PLAYPAL");
    const byte *palette = W_CacheLumpNum(pplump);
    palette += pal * (3 * 256);
    for (int i = 0; i < 256; i++) {
        /* RGB666: expand 6-bit fields to 8 bits (r<<2|r>>4 pattern) */
        s_lut666[i * 3]     = (palette[0] << 2) | (palette[0] >> 4);
        s_lut666[i * 3 + 1] = (palette[1] << 2) | (palette[1] >> 4);
        s_lut666[i * 3 + 2] = (palette[2] << 2) | (palette[2] >> 4);
        palette += 3;
    }
    W_UnlockLumpNum(pplump);
}

void I_PreInitGraphics(void)
{
    if (!s_fb_mux) {
        s_fb_mux = xSemaphoreCreateMutex();
    }
    if (!s_pal_fb) {
        s_pal_fb = heap_caps_malloc(SCREENWIDTH * SCREENHEIGHT,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_666_fb) {
        s_666_fb = heap_caps_aligned_alloc(64, SCREENWIDTH * SCREENHEIGHT * 3,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
}

void I_SetRes(void)
{
    for (int i = 0; i < 3; i++) {
        screens[i].width = SCREENWIDTH;
        screens[i].height = SCREENHEIGHT;
        screens[i].byte_pitch = SCREENPITCH;
        screens[i].short_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE16);
        screens[i].int_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE32);
    }
    screens[4].width = SCREENWIDTH;
    screens[4].height = ST_SCALED_HEIGHT + 1;
    screens[4].byte_pitch = SCREENPITCH;
    screens[4].short_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE16);
    screens[4].int_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE32);

    screens[0].not_on_heap = true;
    screens[0].data = s_pal_fb;
}

void I_InitGraphics(void)
{
    static int firsttime = 1;
    if (firsttime) {
        firsttime = 0;
        I_PreInitGraphics();
        lprintf(LO_INFO, "I_InitGraphics: %dx%d\n", SCREENWIDTH, SCREENHEIGHT);
        I_UpdateVideoMode();
        I_InitInputs();
    }
}

void I_UpdateVideoMode(void)
{
    video_mode_t mode = VID_MODE8;
    V_InitMode(mode);
    V_DestroyUnusedTrueColorPalettes();
    V_FreeScreens();
    I_SetRes();
    V_AllocScreens();
    R_InitBuffer(SCREENWIDTH, SCREENHEIGHT);
}

/* ---- UI-task side ---- */

void doom_video_blit(ili9488_t *lcd, const ui_rect_t *dst)
{
    if (!s_666_fb || lcd == NULL || dst == NULL) {
        return;
    }
    xSemaphoreTake(s_fb_mux, portMAX_DELAY);
    /* PSRAM is DMA-capable on P4 but cache-backed: push our writes out. */
    esp_cache_msync((void *)s_666_fb, SCREENWIDTH * SCREENHEIGHT * 3,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ili9488_blit_rgb666_stream(lcd, s_666_fb, dst->x, dst->y, dst->w, dst->h);
    xSemaphoreGive(s_fb_mux);
}

bool doom_video_take_frame(void)
{
    if (!s_frame_ready) {
        return false;
    }
    s_frame_ready = false;
    return true;
}
