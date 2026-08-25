/* System layer: WAD served from a RAM image of /doomstore/DOOM1.WAD
 * (flash storage partition), timing via gettimeofday, exit hook.
 * Adapted from esp32-doom i_system.c; partition-mmap replaced by the
 * RAM copy so I_Mmap stays zero-copy. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "config.h"
#include "m_argv.h"
#include "lprintf.h"
#include "doomtype.h"
#include "doomdef.h"
#include "m_fixed.h"
#include "r_fps.h"
#include "i_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

int realtime = 0;
void doom_engine_exit_hook(int rc);

static const char *TAG = "doom_sys";

#define DOOM_MOUNT   "/doomstore"
#define DOOM_WADPATH DOOM_MOUNT "/DOOM1.WAD"

static uint8_t *s_wad;
static size_t   s_wad_size;
static wl_handle_t s_wl = WL_INVALID_HANDLE;

esp_err_t doom_wad_load(void)
{
    if (s_wad) {
        return ESP_OK;
    }
    wl_handle_t wl = WL_INVALID_HANDLE;
    esp_vfs_fat_mount_config_t cfg = {
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(DOOM_MOUNT, "storage",
                                                     &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage mount failed: %s", esp_err_to_name(err));
        return err;
    }

    struct stat st;
    if (stat(DOOM_WADPATH, &st) != 0 || st.st_size <= 0) {
        ESP_LOGE(TAG, "DOOM1.WAD not found in storage");
        return ESP_ERR_NOT_FOUND;
    }
    s_wad_size = (size_t)st.st_size;
    s_wad = heap_caps_malloc(s_wad_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_wad) {
        return ESP_ERR_NO_MEM;
    }
    FILE *f = fopen(DOOM_WADPATH, "rb");
    if (!f || fread(s_wad, 1, s_wad_size, f) != s_wad_size) {
        if (f) fclose(f);
        return ESP_FAIL;
    }
    fclose(f);
    ESP_LOGI(TAG, "WAD loaded: %u bytes", (unsigned)s_wad_size);
    return ESP_OK;
}

void doom_wad_unload(void)
{
    if (s_wad) {
        free(s_wad);
        s_wad = NULL;
        s_wad_size = 0;
        esp_vfs_fat_spiflash_unmount_rw_wl(DOOM_MOUNT, s_wl);
        s_wl = WL_INVALID_HANDLE;
    }
}

void I_uSleep(unsigned long usecs)
{
    vTaskDelay(usecs / (portTICK_PERIOD_MS * 1000) + 1);
}

int I_GetTime_RealTime(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int)(tv.tv_sec * TICRATE + (tv.tv_usec * TICRATE) / 1000000);
}

const int displaytime = 0;

fixed_t I_GetTimeFrac(void)
{
    if (tic_vars.step == 0) {
        return FRACUNIT;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    unsigned long now = tv.tv_usec / 1000 + tv.tv_sec * 1000;
    fixed_t frac = (fixed_t)((now - tic_vars.start + displaytime) * FRACUNIT / tic_vars.step);
    if (frac < 0)      { frac = 0; }
    if (frac > FRACUNIT) { frac = FRACUNIT; }
    return frac;
}

void I_GetTime_SaveMS(void)
{
    if (!movement_smooth) {
        return;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    unsigned long now = tv.tv_usec / 1000 + tv.tv_sec * 1000;
    tic_vars.start = now;
    tic_vars.next = (unsigned int)((tic_vars.start * tic_vars.msec + 1.0f) / tic_vars.msec);
    tic_vars.step = tic_vars.next - tic_vars.start;
}

unsigned long I_GetRandomTimeSeed(void) { return 4; }

const char *I_GetVersionString(char *buf, size_t sz)
{
    snprintf(buf, sz, "%s v%s (prboom)", PACKAGE, VERSION);
    return buf;
}

const char *I_SigString(char *buf, size_t sz, int signum) { return buf; }

/* ---- WAD file API over the RAM image ---- */

typedef struct {
    const uint8_t *base;
    int offset;
    int size;
} FileDesc;

static FileDesc fds[32];

int I_Open(const char *wad, int flags)
{
    int x = 3;
    while (fds[x].base != NULL) { x++; }
    if (strcmp(wad, "DOOM1.WAD") == 0 && s_wad) {
        fds[x].base = s_wad;
        fds[x].offset = 0;
        fds[x].size = (int)s_wad_size;
        return x;
    }
    lprintf(LO_INFO, "I_Open: open %s failed\n", wad);
    return -1;
}

int I_Lseek(int ifd, off_t offset, int whence)
{
    if (whence == SEEK_SET)       { fds[ifd].offset = (int)offset; }
    else if (whence == SEEK_CUR)  { fds[ifd].offset += (int)offset; }
    else if (whence == SEEK_END)  { fds[ifd].offset = fds[ifd].size; }
    return fds[ifd].offset;
}

int I_Filelength(int ifd) { return fds[ifd].size; }

void I_Close(int fd) { fds[fd].base = NULL; }

void *I_Mmap(void *addr, size_t length, int prot, int flags, int ifd, off_t offset)
{
    if (!fds[ifd].base) {
        lprintf(LO_ERROR, "I_Mmap: bad fd\n");
        return NULL;
    }
    /* Zero-copy view into the RAM-resident WAD. */
    return (void *)(fds[ifd].base + (int)offset);
}

int I_Munmap(void *addr, size_t length) { return 0; }

void I_Read(int ifd, void *vbuf, size_t sz)
{
    memcpy(vbuf, fds[ifd].base + fds[ifd].offset, sz);
    fds[ifd].offset += (int)sz;
}

const char *I_DoomExeDir(void) { return ""; }

char *I_FindFile(const char *wfname, const char *ext)
{
    return NULL;
}

void I_SetAffinityMask(void) {}

/* I_Quit/I_Error funnel here in the original; route to our task-exit hook. */
void I_SafeExit(int rc)
{
    doom_engine_exit_hook(rc);
}
