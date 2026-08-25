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

#include "ili9488.h"
#include "arpile_ui_core.h"
#include "doom_arpile.h"

int use_fullscreen = 0;
int use_doublebuffer = 0;

static uint8_t  *s_pal_fb;      /* 320x240 palette8, written by engine */
static uint16_t *s_rgb_fb;      /* 320x240 RGB565, converted on frame flip */
static volatile bool s_frame_ready;
static SemaphoreHandle_t s_fb_mux;

/* RGB565 lookup built by I_SetPalette from PLAYPAL */
static uint16_t s_lut[256];

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
    if (!s_pal_fb || !s_rgb_fb || !s_fb_mux) {
        return;
    }
    xSemaphoreTake(s_fb_mux, portMAX_DELAY);
    for (int i = 0; i < SCREENWIDTH * SCREENHEIGHT; i++) {
        s_rgb_fb[i] = s_lut[s_pal_fb[i]];
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
        s_lut[i] = ((palette[0] & 0xF8) << 8) |
                   ((palette[1] & 0xFC) << 3) |
                   (palette[2] >> 3);
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
    if (!s_rgb_fb) {
        s_rgb_fb = heap_caps_malloc(SCREENWIDTH * SCREENHEIGHT * 2,
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
    if (!s_rgb_fb || lcd == NULL || dst == NULL) {
        return;
    }
    xSemaphoreTake(s_fb_mux, portMAX_DELAY);
    ili9488_draw_pixels(lcd, s_rgb_fb, dst->x, dst->y, dst->w, dst->h,
                        0, 0, SCREENWIDTH, SCREENHEIGHT, SCREENWIDTH);
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
