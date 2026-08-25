/* DOOM as a normal Arpile application. Engine runs on its own task; the UI
 * task renders frames through doom_video_blit(); input flows HID→ring. */
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "arpile_app.h"
#include "arpile_ui.h"

#include "doom_arpile.h"

typedef struct {
    bool engine_started;
} doom_state_t;

static void doom_app_init(arpile_app_ctx_t *ctx)
{
    doom_state_t *st = calloc(1, sizeof(doom_state_t));
    ctx->user = st;
}

static void doom_app_update(arpile_app_ctx_t *ctx)
{
    doom_state_t *st = ctx->user;
    if (!st || !ctx->win) {
        return;
    }
    if (!st->engine_started) {
        st->engine_started = true;
        if (doom_engine_start() != ESP_OK) {
            /* No WAD: close back to launcher (deferred, stack-safe). */
            arpile_app_close_win(ctx->win);
            return;
        }
    }
    /* ~25 fps repaint cadence; actual blit happens in render(). */
    static TickType_t last;
    TickType_t now = xTaskGetTickCount();
    if (now - last >= pdMS_TO_TICKS(40)) {
        last = now;
        arpile_ui_win_redraw(ctx->win);
    }
}

static void doom_app_event(arpile_app_ctx_t *ctx, const arpile_input_event_t *ev)
{
    if (ev->type != ARPILE_IN_EVENT_KEY_DOWN &&
        ev->type != ARPILE_IN_EVENT_KEY_UP) {
        return;
    }
    extern int doom_input_translate(uint16_t keycode);
    int k = doom_input_translate(ev->key.keycode);
    if (k) {
        doom_input_post(ev->type == ARPILE_IN_EVENT_KEY_DOWN, k);
    }
}

static void doom_app_render(arpile_app_ctx_t *ctx, ui_win_t *win)
{
    ili9488_t *lcd = arpile_ui_get_lcd();
    if (!lcd) {
        return;
    }
    ui_rect_t c = win_client_rect(win);
    ui_draw_fill_rect(lcd, &c, 0x0000);          /* letterbox base */
    doom_video_blit(lcd, &c);                    /* scaled frame on top */
}

static void doom_app_destroy(arpile_app_ctx_t *ctx)
{
    if (ctx->user && ((doom_state_t *)ctx->user)->engine_started) {
        doom_engine_stop();
        extern void doom_wad_unload(void);
        doom_wad_unload();
    }
    free(ctx->user);
    ctx->user = NULL;
}

static const arpile_app_ops_t doom_ops = {
    .init = doom_app_init,
    .update = doom_app_update,
    .event = doom_app_event,
    .render = doom_app_render,
    .destroy = doom_app_destroy,
};

const arpile_app_t arpile_app_doom_app = {
    .id = "doom_app",
    .name = "DOOM",
    .icon = "doom",
    .ops = &doom_ops,
};
