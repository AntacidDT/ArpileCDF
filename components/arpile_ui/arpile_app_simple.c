/* Small placeholder applications. Each just shows its icon + name, so the
 * launcher grid is populated with the app logos until real backends exist. */
#include <stdio.h>
#include <string.h>
#include "arpile_app.h"
#include "arpile_ui.h"

typedef struct {
    char msg[64];
} simple_state_t;

static void simple_init(arpile_app_ctx_t *ctx)
{
    simple_state_t *st = calloc(1, sizeof(simple_state_t));
    snprintf(st->msg, sizeof(st->msg), "%s coming soon", ctx->app->name);
    ctx->user = st;
}

static void simple_render(arpile_app_ctx_t *ctx, ui_win_t *win)
{
    simple_state_t *st = ctx->user;
    if (!st) return;
    ili9488_t *lcd = arpile_ui_get_lcd();
    ui_rect_t c = win_client_rect(win);
    ui_draw_fill_rect(lcd, &c, UI_C_WIN_BG);
    if (ctx->app->icon) {
        int cx = c.x + (c.w - 16) / 2;
        int cy = c.y + (c.h - 16) / 2 - 8;
        if (!ui_draw_icon_img(lcd, ctx->app->icon, (uint16_t)cx, (uint16_t)cy, 16)) {
            ui_draw_icon(lcd, ctx->app->icon, (uint16_t)cx, (uint16_t)cy,
                         UI_C_ACCENT, UI_C_WIN_BG);
        }
    }
    uint16_t tw = ui_text_width(st->msg);
    ui_draw_text(lcd, (uint16_t)(c.x + (c.w - tw) / 2),
                 (uint16_t)(c.y + c.h / 2 + 8),
                 st->msg, UI_C_TEXT, UI_C_WIN_BG);
}

static void simple_destroy(arpile_app_ctx_t *ctx)
{
    if (ctx->user) {
        free(ctx->user);
        ctx->user = NULL;
    }
}

static const arpile_app_ops_t simple_ops = {
    .init = simple_init,
    .render = simple_render,
    .destroy = simple_destroy,
};

#define SIMPLE_APP(id_, name_, icon_)                                          \
    const arpile_app_t arpile_app_##id_ = {                                    \
        .id = #id_, .name = name_, .icon = icon_, .ops = &simple_ops,          \
    };

SIMPLE_APP(doom_app, "DOOM", "doom")
SIMPLE_APP(voxel, "Voxel", "voxel")
SIMPLE_APP(slotsim, "Slot", "slotsim")