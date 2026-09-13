/* Voxel 0.1: interactive textured-3D cube renderer on the Arpile desktop.
 *
 * Gameplay UI layer only: a discrete FPS counter (top-left) and a compact
 * Minecraft-style block hotbar (bottom-centre). World/gen/lighting/camera
 * logic lives in voxel_game.c; this file only reads game state. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "arpile_app.h"
#include "arpile_ui.h"
#include "esp_cache.h"
#include "voxel_game.h"
#include "voxel_renderer.h"
#include "voxel_block.h"
#include "voxel_texture.h"

/* ---- hotbar block icons ---------------------------------------------
 * Each slot icon is a tiny isometric cube rendered from the world's own
 * procedurally generated tile atlas (no external assets), shaded like the
 * in-game faces. */
#define VOX_ICON_SZ 22
#define ICON_CX     10   /* cube centre column */
#define ICON_TOP     4   /* y of the cube's top vertex */
#define ICON_HALF_W  8   /* half-width of the diamond top face */
#define ICON_HALF_H  4   /* top face vertical half-height */
#define ICON_DROP    5   /* side-face depth in px */

typedef struct {
    vox_game *game;
    TickType_t last_update;
    TickType_t last_paint;
    bool show_timing;
    uint16_t icon[VOX_HOTBAR_SLOTS][VOX_ICON_SZ * VOX_ICON_SZ];
} vox_state_t;

static uint16_t tint_rgb565(uint16_t p, int scale)
{
    int r = (p >> 11) & 0x1F;
    int g = (p >> 5) & 0x3F;
    int b = p & 0x1F;
    r = (r * scale) >> 8;
    g = (g * scale) >> 8;
    b = (b * scale) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void icon_sample(const vox_tex_atlas *atlas, vox_tex_id tex,
                        int u, int v, int shade, uint16_t *pix)
{
    *pix = tint_rgb565(vox_tex_sample(atlas, tex, u, v), shade);
}

/* Render a 3/4-view cube into `out` using the world's own procedural atlas
 * tiles: bright top face, mid left side, dark right side (matching the
 * in-game WORLD_LIGHT shading). */
static void gen_block_icon(const vox_tex_atlas *atlas, vox_block_id block,
                           uint16_t out[VOX_ICON_SZ * VOX_ICON_SZ])
{
    vox_tex_id t_top   = vox_block_face_tex(block, VOX_FACE_Y_POS);
    vox_tex_id t_left  = vox_block_face_tex(block, VOX_FACE_X_NEG);
    vox_tex_id t_right = vox_block_face_tex(block, VOX_FACE_X_POS);
    const int sh_top   = 255;
    const int sh_left  = 164;
    const int sh_right = 115;

    for (int py = 0; py < VOX_ICON_SZ; py++) {
        int ty = py - ICON_TOP;
        for (int px = 0; px < VOX_ICON_SZ; px++) {
            uint16_t *outp = &out[py * VOX_ICON_SZ + px];
            int dx = px - ICON_CX;

            if (ty >= 0 && ty <= 2 * ICON_HALF_H) {
                int dy = ty - ICON_HALF_H;                /* -H..H */
                int hw = ICON_HALF_W - ICON_HALF_W * abs(dy) / ICON_HALF_H;
                if (hw > 0 && dx >= -hw && dx <= hw) {
                    int u = ((dx + hw) * 15) / (2 * hw);
                    int v = (ty * 15) / (2 * ICON_HALF_H);
                    icon_sample(atlas, t_top, u, v, sh_top, outp);
                    continue;
                }
            }

            if (ty >= ICON_HALF_H && ty <= ICON_HALF_H + ICON_DROP) {
                int t = ty - ICON_HALF_H;                 /* 0..DROP */
                int v = (t * 15) / ICON_DROP;
                /* side faces hang from the diamond's side vertices: outer
                 * (back) edge is vertical at CX+/-HALF_W, the inner (front)
                 * edge runs L->B (left) and R->B (right) then straight down */
                if (dx < 0 && px >= ICON_CX - ICON_HALF_W) {
                    int u = ((px - (ICON_CX - ICON_HALF_W)) * 15) / ICON_HALF_W;
                    icon_sample(atlas, t_left, u, v, sh_left, outp);
                } else if (dx > 0 && px <= ICON_CX + ICON_HALF_W) {
                    int u = (((ICON_CX + ICON_HALF_W) - px) * 15) / ICON_HALF_W;
                    icon_sample(atlas, t_right, u, v, sh_right, outp);
                }
            }
        }
    }
}

static void build_hotbar_icons(vox_state_t *st)
{
    const vox_tex_atlas *atlas = vox_game_atlas(st->game);
    if (!atlas) return;
    for (int i = 0; i < VOX_HOTBAR_SLOTS; i++) {
        vox_block_id b = vox_game_slot_block(st->game, i);
        if (b == VOX_BLOCK_AIR) continue;
        gen_block_icon(atlas, b, st->icon[i]);
    }
}

/* ---- app lifecycle -------------------------------------------------- */

static void voxel_app_init(arpile_app_ctx_t *ctx)
{
    vox_state_t *st = calloc(1, sizeof(vox_state_t));
    if (!st) return;
    st->game = vox_game_create();
    st->last_update = xTaskGetTickCount();
    st->last_paint = 0;
    ctx->user = st;
    if (st->game) {
        build_hotbar_icons(st);
    }
}

static void voxel_app_update(arpile_app_ctx_t *ctx)
{
    vox_state_t *st = ctx->user;
    if (!st || !st->game || !ctx->win) return;

    TickType_t now = xTaskGetTickCount();
    float dt = (float)(now - st->last_update) * 1000.0f / configTICK_RATE_HZ / 1000.0f;
    st->last_update = now;
    vox_game_update(st->game, dt);

    if (now - st->last_paint >= pdMS_TO_TICKS(50)) {
        st->last_paint = now;
        arpile_ui_win_redraw(ctx->win);
    }
}

static void select_slot_relative(vox_state_t *st, int delta)
{
    if (!st || !st->game) return;
    int sel = vox_game_selected_slot(st->game) + delta;
    sel %= VOX_HOTBAR_SLOTS;
    if (sel < 0) sel += VOX_HOTBAR_SLOTS;
    vox_game_set_slot(st->game, sel);
}

static void voxel_app_event(arpile_app_ctx_t *ctx, const arpile_input_event_t *ev)
{
    vox_state_t *st = ctx->user;
    if (!st || !st->game) return;

    if (ev->type == ARPILE_IN_EVENT_MOUSE_WHEEL) {
        select_slot_relative(st, -ev->wheel);
        if (ctx->win) arpile_ui_win_redraw(ctx->win);
        return;
    }
    if (ev->type == ARPILE_IN_EVENT_MOUSE_MOVE) {
        /* Mouse-look: raw driver deltas -> yaw/pitch (see vox_game_mouse). */
        vox_game_mouse(st->game, ev->mouse.x, ev->mouse.y);
        if (ctx->win) arpile_ui_win_redraw(ctx->win);
        return;
    }
    if (ev->type == ARPILE_IN_EVENT_MOUSE_BTN) {
        return;   /* hotbar has no pointer interaction yet (placing is later) */
    }
    if (ev->type == ARPILE_IN_EVENT_KEY_UP) {
        vox_game_key(st->game, ev->key.keycode, false);
        return;
    }
    if (ev->type != ARPILE_IN_EVENT_KEY_DOWN) {
        return;
    }
    if (ev->key.keycode >= ARPILE_KEY_1 && ev->key.keycode <= ARPILE_KEY_8) {
        vox_game_set_slot(st->game, ev->key.keycode - ARPILE_KEY_1);
        if (ctx->win) arpile_ui_win_redraw(ctx->win);
        return;
    }
    switch (ev->key.keycode) {
    case ARPILE_KEY_F1:
        st->show_timing = !st->show_timing;
        break;
    case ARPILE_KEY_F2:
        vox_game_toggle_spin(st->game);   /* debug-only camera orbit */
        break;
    case ARPILE_KEY_ESCAPE:
        arpile_app_close(ctx);
        break;
    case ARPILE_KEY_PGUP:
        select_slot_relative(st, -1);   /* item before */
        if (ctx->win) arpile_ui_win_redraw(ctx->win);
        break;
    case ARPILE_KEY_PGDN:
        select_slot_relative(st, +1);   /* item next */
        if (ctx->win) arpile_ui_win_redraw(ctx->win);
        break;
    case ARPILE_KEY_SPACE:
        vox_game_key(st->game, ARPILE_KEY_SPACE, true);   /* jump */
        break;
    default:
        vox_game_key(st->game, ev->key.keycode, true);
        break;
    }
}

/* ---- rendering -------------------------------------------------------- */

static void draw_hotbar(vox_state_t *st, ili9488_t *lcd, ui_rect_t c)
{
    int sel = vox_game_selected_slot(st->game);
    const uint16_t slot_bg = UI_RGB(0x0E, 0x13, 0x1C);
    const uint16_t slot_border = UI_RGB(0x5A, 0x7C, 0x9E);
    const uint16_t sel_col = UI_C_ACCENT;

    const int slot = 26;
    const int gap = 3;
    const int margin = 3;
    int total = VOX_HOTBAR_SLOTS * slot + (VOX_HOTBAR_SLOTS - 1) * gap + 2 * margin;
    int x0 = (int)c.x + (int)(c.w - (uint16_t)total) / 2;
    int y0 = (int)c.y + (int)c.h - slot - 4;

    char numbuf[2] = { 0, 0 };

    for (int i = 0; i < VOX_HOTBAR_SLOTS; i++) {
        int sx = x0 + margin + i * (slot + gap);
        ui_rect_t cell = { (uint16_t)sx, (uint16_t)y0, (uint16_t)slot, (uint16_t)slot };
        ui_draw_fill_rect(lcd, &cell, slot_bg);

        vox_block_id b = vox_game_slot_block(st->game, i);
        if (b != VOX_BLOCK_AIR) {
            int inset = (slot - VOX_ICON_SZ) / 2;
            ui_fb_blit(st->icon[i],
                       (uint16_t)(sx + inset), (uint16_t)(y0 + inset),
                       VOX_ICON_SZ, VOX_ICON_SZ,
                       0, 0, VOX_ICON_SZ, VOX_ICON_SZ, VOX_ICON_SZ);
        }

        /* slot number in the corner (Minecraft-style), over the dark cell */
        numbuf[0] = (char)('1' + i);
        ui_draw_text(lcd, (uint16_t)(sx + 1), (uint16_t)(y0 + 1), numbuf,
                     UI_C_TEXT_LIGHT, slot_bg);

        if (i == sel) {
            /* protruding selector: outer + inner accent frames + nub */
            ui_draw_outline(lcd, &cell, sel_col);
            ui_draw_outline(lcd, &(ui_rect_t){ (uint16_t)(sx - 1), (uint16_t)(y0 - 1),
                                               (uint16_t)(slot + 2), (uint16_t)(slot + 2) },
                            sel_col);
            for (int k = 0; k < 3; k++) {
                ui_draw_hline(lcd, (uint16_t)(sx + slot / 2 - k),
                              (uint16_t)(y0 - 3 + k), (uint16_t)(1 + 2 * k), sel_col);
            }
        } else {
            ui_draw_outline(lcd, &cell, slot_border);
        }
    }
}

static void voxel_app_render(arpile_app_ctx_t *ctx, ui_win_t *win)
{
    vox_state_t *st = ctx->user;
    if (!st || !st->game) return;
    ili9488_t *lcd = arpile_ui_get_lcd();
    if (!lcd) return;

    ui_rect_t c = win_client_rect(win);
    if (c.w < 2 || c.h < 2) return;

    vox_renderer *r = vox_game_renderer(st->game);
    if (vox_renderer_resize(r, c.w, c.h) != 0) return;

    vox_game_render(st->game);

    esp_cache_msync(r->fb, (size_t)r->width * r->height * sizeof(uint16_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ui_fb_blit(r->fb, c.x, c.y, c.w, c.h,
               0, 0, (uint16_t)r->width, (uint16_t)r->height,
               (uint16_t)r->width);

    /* Top-left: FPS only (the one permanent stat). */
    char buf[128];
    vox_game_stats gs;
    vox_game_get_stats(st->game, &gs);
    snprintf(buf, sizeof(buf), "FPS: %d", gs.fps);
    ui_draw_fill_rect(lcd, &(ui_rect_t){ c.x + 2, (uint16_t)(c.y + 2),
                                         (uint16_t)(ui_text_width(buf) + 8), CHAR_H }, 0x0000);
    ui_draw_text(lcd, (uint16_t)(c.x + 6), (uint16_t)(c.y + 4), buf, 0xFFFF, 0x0000);

    draw_hotbar(st, lcd, c);

    /* Debug stats (F1): one compact line, bottom-left, out of the way. Also
     * shows the player origin/ground state to verify physics on hardware. */
    if (st->show_timing) {
        uint32_t ocl = gs.faces_culled;
        uint32_t fr = gs.faces_drawn + ocl;
        uint32_t pct = (fr > 0) ? (uint32_t)((float)ocl * 100.0f / (float)fr) : 0;
        vox_game_player_state pi;
        vox_game_player_info(st->game, &pi);
        uint16_t ly = (uint16_t)(c.y + c.h - CHAR_H - 4);
        snprintf(buf, sizeof(buf), "T %d c%u%% r%u f%u p %.0f/%.1f/%.0f %s",
                 gs.tris, (unsigned)pct,
                 (unsigned)gs.raster_us, (unsigned)gs.frame_us,
                 pi.pos.x, pi.pos.y, pi.pos.z, pi.grounded ? "G" : "A");
        ui_draw_fill_rect(lcd, &(ui_rect_t){ c.x + 2, ly,
                                             (uint16_t)(ui_text_width(buf) + 8), CHAR_H }, 0x0000);
        ui_draw_text(lcd, (uint16_t)(c.x + 6), (uint16_t)(ly + 2), buf, 0xFFFF, 0x0000);
    }
}

static void voxel_app_destroy(arpile_app_ctx_t *ctx)
{
    vox_state_t *st = ctx->user;
    if (st) {
        vox_game_destroy(st->game);
        free(st);
        ctx->user = NULL;
    }
}

static const arpile_app_ops_t voxel_ops = {
    .init = voxel_app_init,
    .update = voxel_app_update,
    .event = voxel_app_event,
    .render = voxel_app_render,
    .destroy = voxel_app_destroy,
};

const arpile_app_t arpile_app_voxel = {
    .id = "voxel",
    .name = "Voxel",
    .icon = "voxel",
    .ops = &voxel_ops,
};