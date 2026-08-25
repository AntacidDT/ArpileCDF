/* Engine task entry — adapted from esp32-doom main/app_main.c and its
 * compat i_main.c (timing machinery). Hardware init stays in Arpile. */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "doomtype.h"
#include "d_main.h"
#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"

#include "doom_arpile.h"

static const char *TAG = "doom";

static TaskHandle_t s_engine_task;
static volatile bool s_stop_req;
static SemaphoreHandle_t s_done;

void doom_engine_exit_hook(int rc)
{
    ESP_LOGI(TAG, "engine exit(%d)", rc);
    xSemaphoreGive(s_done);
    s_engine_task = NULL;
    vTaskDelete(NULL);
    for (;;) { } /* not reached */
}

/* ---- timing machinery (original compat layer) ---- */
#include "m_fixed.h"
#include "r_fps.h"

int realtic_clock_rate = 100;
static int_64_t I_GetTime_Scale = 1 << 24;

static int I_GetTime_Scaled(void)
{
    return (int)(((int_64_t)I_GetTime_RealTime() * I_GetTime_Scale) >> 24);
}

static int fastdemo_tic;
static int I_GetTime_FastDemo(void) { return fastdemo_tic++; }

static int I_GetTime_Error(void)
{
    I_Error("I_GetTime_Error: GetTime() used before initialization");
    return 0;
}

int (*I_GetTime)(void) = I_GetTime_Error;

void I_Init(void)
{
    if (fastdemo) {
        I_GetTime = I_GetTime_FastDemo;
    } else if (realtic_clock_rate != 100) {
        I_GetTime_Scale = ((int_64_t)realtic_clock_rate << 24) / 100;
        I_GetTime = I_GetTime_Scaled;
    } else {
        I_GetTime = I_GetTime_RealTime;
    }
    if (!(nomusicparm && nosfxparm)) {
        I_InitSound();
    }
    R_InitInterpolation();
}

/* Globals originally defined by the esp32-doom compat layer. */
unsigned int endoom_mode;
int usejoystick = 0, joyleft, joyright, joyup, joydown;

/* ---- engine entry (was main() wrapper in original compat layer) ---- */

int doom_main(int argc, char const *const *argv)
{
    myargc = argc;
    myargv = argv;
    D_DoomMain();
    return 0;
}

static void doom_engine_task(void *arg)
{
    /* PrBoom init/game prints flood the file_xfer console tap, whose TX queue
     * then wedges both cores. Engine chatter goes to the void; system logs
     * (ESP_LOGx) are a separate path and stay visible. */
    if (!freopen("/dev/null", "w", stdout)) {
        ESP_LOGW(TAG, "stdout redirect failed");
    }
    setvbuf(stdout, NULL, _IONBF, 0);

    char const *argv[] = { "doom", "-cout", "ICWEFDA", NULL };
    doom_main(3, argv);
    doom_engine_exit_hook(0);   /* clean end without I_Quit */
}

esp_err_t doom_engine_start(void)
{
    if (s_engine_task) {
        return ESP_ERR_INVALID_STATE;
    }
    s_stop_req = false;
    s_done = xSemaphoreCreateBinary();

    extern esp_err_t doom_wad_load(void);
    ESP_LOGI(TAG, "loading WAD...");
    esp_err_t err = doom_wad_load();
    ESP_LOGI(TAG, "WAD load: %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DOOM1.WAD load failed: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_done);
        s_done = NULL;
        return err;
    }

    /* Unpinned + low priority: PrBoom's init/game loop is long pure-compute
     * stretches; pinning/starving it above system tasks froze USB HID. */
    if (xTaskCreate(doom_engine_task, "doomEngine", 28672,
                    NULL, 3, &s_engine_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void doom_engine_stop(void)
{
    if (!s_engine_task) {
        return;
    }
    /* Walk the menu back, then press the quit key so I_Quit runs cleanly. */
    extern int key_escape, key_quit;
    for (int i = 0; i < 4 && eTaskGetState(s_engine_task) != eDeleted; i++) {
        doom_input_post(true, key_escape);
        vTaskDelay(pdMS_TO_TICKS(60));
        doom_input_post(false, key_escape);
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    doom_input_post(true, key_quit);
    vTaskDelay(pdMS_TO_TICKS(100));
    doom_input_post(false, key_quit);
    xSemaphoreTake(s_done, pdMS_TO_TICKS(3000));
    s_engine_task = NULL;
    if (s_done) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
}

bool doom_engine_running(void)
{
    return s_engine_task != NULL;
}
