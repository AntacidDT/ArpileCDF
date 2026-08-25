/* CAW — Cow And Wheat. Keyboard-only catch game.
 * The cow catches falling wheat (+10) and dodges crows (-1 life).
 * ENTER starts/restarts, ESC exits back to the launcher. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "arpile_app.h"
#include "arpile_ui.h"

#define CAW_MAX_ITEMS   12
#define COW_W           28
#define COW_H           18
#define ITEM_W          10
#define ITEM_H          14

/* Palette additions (RGB565) */
#define C_SKY     UI_RGB(0x18, 0x2A, 0x45)
#define C_GRASS   UI_RGB(0x2E, 0x7D, 0x32)
#define C_GRASS_D UI_RGB(0x1B, 0x5E, 0x20)
#define C_COW     UI_RGB(0xF5, 0xF5, 0xF0)
#define C_SPOT    UI_RGB(0x21, 0x21, 0x21)
#define C_SNOUT   UI_RGB(0xF0, 0x9A, 0x9A)
#define C_WHEAT   UI_RGB(0xE6, 0xC2, 0x29)
#define C_STRAW   UI_RGB(0xC4, 0x9A, 0x1C)
#define C_CROW    UI_RGB(0x10, 0x10, 0x10)
#define C_BEAK    UI_RGB(0xFF, 0x98, 0x00)
#define C_HEART   UI_RGB(0xE5, 0x39, 0x35)
#define C_GOLD    UI_RGB(0xFF, 0xD5, 0x4F)

typedef enum { CAW_MENU, CAW_PLAY, CAW_OVER } caw_mode_t;

typedef struct {
    int16_t x, y;
    bool crow;
    bool active;
} caw_item_t;

typedef struct {
    caw_mode_t mode;
    int score;
    int lives;
    int16_t cow_x;            /* left edge of cow */
    uint16_t field_w, field_h;
    caw_item_t items[CAW_MAX_ITEMS];
    TickType_t last_frame;
    TickType_t next_spawn;
    int fall_speed;           /* px per frame */
    /* input edge/held tracking (set in HID context, consumed on UI task) */
    volatile bool hold_l, hold_r;
} caw_state_t;

static void caw_init(arpile_app_ctx_t *ctx)
{
    caw_state_t *st = calloc(1, sizeof(caw_state_t));
    if (!st) return;
    st->mode = CAW_MENU;
    st->lives = 3;
    st->fall_speed = 4;
    ctx->user = st;
    /* Arrows normally drive the mouse cursor globally; opt in so Left/Right
     * reach this window's key handler while CAW is focused. */
    if (ctx->win) {
        ctx->win->state.capture_nav = true;
    }
}

static void caw_reset(caw_state_t *st, ui_win_t *win)
{
    ui_rect_t c = win_client_rect(win);
    memset(st->items, 0, sizeof(st->items));
    st->score = 0;
    st->lives = 3;
    st->fall_speed = 4;
    st->cow_x = (int16_t)((c.w - COW_W) / 2);
    st->field_w = c.w;
    st->field_h = c.h;
    st->next_spawn = xTaskGetTickCount();
}

static void caw_start(caw_state_t *st, ui_win_t *win)
{
    caw_reset(st, win);
    st->mode = CAW_PLAY;
    arpile_ui_win_redraw(win);
}

static void caw_update(arpile_app_ctx_t *ctx)
{
    caw_state_t *st = ctx->user;
    if (!st || st->mode != CAW_PLAY || !ctx->win) {
        return;
    }
    TickType_t now = xTaskGetTickCount();
    if (now - st->last_frame < pdMS_TO_TICKS(33)) {
        return;                       /* ~30 fps cap */
    }
    st->last_frame = now;

    ui_rect_t c = win_client_rect(ctx->win);
    st->field_w = c.w;
    st->field_h = c.h;

    /* cow movement (held keys) */
    if (st->hold_l && st->cow_x > 2) {
        st->cow_x -= 9;
    }
    if (st->hold_r && st->cow_x < (int16_t)(c.w - COW_W - 2)) {
        st->cow_x += 9;
    }

    /* spawn */
    if (now >= st->next_spawn) {
        for (int i = 0; i < CAW_MAX_ITEMS; i++) {
            if (!st->items[i].active) {
                st->items[i].active = true;
                st->items[i].crow = (st->score >= 50 && (rand() % 100) < 25);
                st->items[i].x = (int16_t)(rand() % (c.w - ITEM_W));
                st->items[i].y = -ITEM_H;
                break;
            }
        }
        int ms = 620 - (st->score / 20) * 55;
        if (ms < 240) {
            ms = 240;
        }
        st->next_spawn = now + pdMS_TO_TICKS(ms);
    }

    /* fall + collide */
    int16_t cow_top = (int16_t)(c.h - 22 - COW_H);
    for (int i = 0; i < CAW_MAX_ITEMS; i++) {
        caw_item_t *it = &st->items[i];
        if (!it->active) {
            continue;
        }
        it->y = (int16_t)(it->y + st->fall_speed);
        bool hit_cow = it->y + ITEM_H >= cow_top &&
                       it->y <= cow_top + COW_H &&
                       it->x + ITEM_W >= st->cow_x &&
                       it->x <= st->cow_x + COW_W;
        if (hit_cow) {
            it->active = false;
            if (it->crow) {
                if (--st->lives <= 0) {
                    st->mode = CAW_OVER;
                    arpile_ui_win_redraw(ctx->win);
                    return;
                }
            } else {
                st->score += 10;
                if (st->score % 80 == 0 && st->fall_speed < 9) {
                    st->fall_speed++;
                }
            }
        } else if (it->y > (int16_t)c.h) {
            it->active = false;
        }
    }

    arpile_ui_win_redraw(ctx->win);
}

static void draw_cow(ili9488_t *lcd, int16_t x, int16_t y)
{
    ui_draw_fill_rect(lcd, &(ui_rect_t){ (uint16_t)(x + 4), (uint16_t)y, 22, 12 }, C_COW);
    ui_draw_fill_rect(lcd, &(ui_rect_t){ (uint16_t)x, (uint16_t)(y + 4), 10, 10 }, C_COW);
    ui_draw_fill_rect(lcd, &(ui_rect_t){ (uint16_t)x, (uint16_t)(y + 4), 10, 5 }, C_SPOT);
    ui_draw_fill_rect(lcd, &(ui_rect_t){ (uint16_t)(x + 14), (uint16_t)(y + 3), 5, 4 }, C_SPOT);
    ui_draw_fill_rect(lcd, &(ui_rect_t){ (uint16_t)x, (uint16_t)(y + 11), 8, 3 }, C_SNOUT);
    ui_draw_fill_rect(lcd, &(ui_rect_t){ (uint16_t)(x + 7), (uint16_t)(y + 6), 2, 2 }, C_SPOT);
    ui_draw_fill_rect(lcd, &(ui_rect_t){ (uint16_t)(x + 6), (uint16_t)(y + 14), 3, 4 }, C_SPOT);
    ui_draw_fill_rect(lcd, &(ui_rect_t){ (uint16_t)(x + 19), (uint16_t)(y + 14), 3, 4 }, C_SPOT);
}

static void draw_wheat(ili9488_t *lcd, int16_t x, int16_t y)
{
    ui_draw_fill_rect(lcd, &(ui_rect_t){ (uint16_t)(x + 4), (uint16_t)(y + 6), 2, 8 }, C_STRAW);
    ui_draw_fill_rect(lcd, &(ui_rect_t){ (uint16_t)(x + 2), (uint16_t)y, 6, 6 }, C_WHEAT);
    ui_draw_fill_rect(lcd, &(ui_rect_t){ (uint16_t)x, (uint16_t)(y + 2), 2, 3 }, C_WHEAT);
    ui_draw_fill_rect(lcd, &(ui_rect_t){ (uint16_t)(x + 8), (uint16_t)(y + 2), 2, 3 }, C_WHEAT);
}

static void draw_crow(ili9488_t *lcd, int16_t x, int16_t y)
{
    ui_draw_fill_rect(lcd, &(ui_rect_t){ (uint16_t)(x + 2), (uint16_t)(y + 4), 8, 5 }, C_CROW);
    ui_draw_fill_rect(lcd, &(ui_rect_t){ (uint16_t)(x + 4), (uint16_t)y, 4, 5 }, C_CROW);
    ui_draw_fill_rect(lcd, &(ui_rect_t){ (uint16_t)x, (uint16_t)(y + 2), 4, 3 }, C_BEAK);
    ui_draw_fill_rect(lcd, &(ui_rect_t){ (uint16_t)(x + 9), (uint16_t)(y + 5), 3, 2 }, C_CROW);
}

static void draw_hud(ili9488_t *lcd, const caw_state_t *st, const ui_rect_t *c)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "SCORE %d", st->score);
    ui_draw_text(lcd, (uint16_t)(c->x + 4), (uint16_t)(c->y + 2),
                 buf, UI_C_TEXT, C_SKY);
    for (int i = 0; i < st->lives; i++) {
        ui_rect_t h = { (uint16_t)(c->x + c->w - 12 - i * 12), (uint16_t)(c->y + 2), 8, 8 };
        ui_draw_fill_rect(lcd, &h, C_HEART);
    }
}

static void caw_render(arpile_app_ctx_t *ctx, ui_win_t *win)
{
    caw_state_t *st = ctx->user;
    if (!st) return;
    ili9488_t *lcd = arpile_ui_get_lcd();
    ui_rect_t c = win_client_rect(win);

    ui_draw_fill_rect(lcd, &c, C_SKY);
    /* grass strip along the bottom */
    int gh = 22;
    ui_rect_t g = { c.x, (uint16_t)(c.y + c.h - gh), c.w, (uint16_t)gh };
    ui_draw_fill_rect(lcd, &g, C_GRASS);
    for (uint16_t gx = 0; gx < c.w; gx += 8) {
        ui_draw_vline(lcd, (uint16_t)(g.x + gx), (uint16_t)(g.y + gh - 5), 5, C_GRASS_D);
    }

    if (st->mode == CAW_MENU || st->mode == CAW_OVER) {
        const char *title = "C A W";
        const char *sub = "Cow And Wheat";
        uint16_t tx = (uint16_t)(c.x + (c.w - ui_text_width(title)) / 2);
        uint16_t ty = (uint16_t)(c.y + c.h / 2 - 40);
        ui_draw_text(lcd, tx, ty, title, C_GOLD, C_SKY);
        tx = (uint16_t)(c.x + (c.w - ui_text_width(sub)) / 2);
        ui_draw_text(lcd, tx, (uint16_t)(ty + 18), sub, UI_C_TEXT_DIM, C_SKY);
        draw_cow(lcd, (int16_t)(c.x + c.w / 2 - COW_W / 2), (int16_t)(c.y + ty + 40));

        const char *line = (st->mode == CAW_MENU)
                           ? "ENTER start   ESC exit"
                           : "GAME OVER   ENTER restart";
        if (st->mode == CAW_OVER) {
            char sb[32];
            snprintf(sb, sizeof(sb), "Score: %d", st->score);
            uint16_t sx = (uint16_t)(c.x + (c.w - ui_text_width(sb)) / 2);
            ui_draw_text(lcd, sx, (uint16_t)(ty + 66), sb, C_GOLD, C_SKY);
        }
        uint16_t lx = (uint16_t)(c.x + (c.w - ui_text_width(line)) / 2);
        ui_draw_text(lcd, lx, (uint16_t)(c.y + c.h - gh - 22), line, UI_C_TEXT, C_SKY);
        return;
    }

    for (int i = 0; i < CAW_MAX_ITEMS; i++) {
        if (!st->items[i].active) continue;
        if (st->items[i].crow) {
            draw_crow(lcd, (int16_t)(c.x + st->items[i].x), (int16_t)(c.y + st->items[i].y));
        } else {
            draw_wheat(lcd, (int16_t)(c.x + st->items[i].x), (int16_t)(c.y + st->items[i].y));
        }
    }
    draw_cow(lcd, (int16_t)(c.x + st->cow_x), (int16_t)(c.y + c.h - 22 - COW_H));
    draw_hud(lcd, st, &c);
}

static void caw_event(arpile_app_ctx_t *ctx, const arpile_input_event_t *ev)
{
    caw_state_t *st = ctx->user;
    if (!st || ev->type != ARPILE_IN_EVENT_KEY_DOWN) {
        if (st && ev->type == ARPILE_IN_EVENT_KEY_UP) {
            switch (ev->key.keycode) {
            case ARPILE_KEY_LEFT: case ARPILE_KEY_A: st->hold_l = false; break;
            case ARPILE_KEY_RIGHT: case ARPILE_KEY_D: st->hold_r = false; break;
            default: break;
            }
        }
        return;
    }
    switch (ev->key.keycode) {
    case ARPILE_KEY_LEFT: case ARPILE_KEY_A:
        st->hold_l = true;
        break;
    case ARPILE_KEY_RIGHT: case ARPILE_KEY_D:
        st->hold_r = true;
        break;
    case ARPILE_KEY_ENTER: case ARPILE_KEY_SPACE:
        if (st->mode != CAW_PLAY && ctx->win) {
            caw_start(st, ctx->win);
        }
        break;
    case ARPILE_KEY_ESCAPE:
        arpile_app_close_win(ctx->win);   /* deferred close back to launcher */
        break;
    default:
        break;
    }
}

static void caw_destroy(arpile_app_ctx_t *ctx)
{
    free(ctx->user);
    ctx->user = NULL;
}

static const arpile_app_ops_t caw_ops = {
    .init = caw_init,
    .update = caw_update,
    .event = caw_event,
    .render = caw_render,
    .destroy = caw_destroy,
};

const arpile_app_t arpile_app_cowandwheat = {
    .id = "cowandwheat",
    .name = "CAW",
    .icon = "cowandwheat",
    .ops = &caw_ops,
};
