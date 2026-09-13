/* Slot Machine Simulator — self-contained ArpileCDF application.
 * Virtual credits only; no real-money functionality.
 *
 * Layout: left half = reels + controls, right half = info/stats. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "arpile_app.h"
#include "arpile_ui.h"
#include "arpile_ui_draw.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ------------------------------------------------------------------ */
/* Symbols, paytable, weights                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    SYM_CHERRY, SYM_LEMON, SYM_ORANGE, SYM_PLUM,
    SYM_BELL,   SYM_BAR,   SYM_SEVEN,  SYM_STAR,
    SYM_COUNT
} slot_sym_t;

typedef struct { slot_sym_t sym; const char *name; uint16_t color; } sym_info_t;

static const sym_info_t s_sym_info[SYM_COUNT] = {
    { SYM_CHERRY, "CHERRY", 0xF800 },
    { SYM_LEMON,  "LEMON",  0xFFE0 },
    { SYM_ORANGE, "ORANGE", 0xFD20 },
    { SYM_PLUM,   "PLUM",   0xC21F },
    { SYM_BELL,   "BELL",   0xFFA0 },
    { SYM_BAR,    "BAR",    0xFFFF },
    { SYM_SEVEN,  "777",    0xF800 },
    { SYM_STAR,   "STAR",   0xFFE0 },
};

typedef struct { slot_sym_t sym; uint16_t weight; } weight_entry_t;

static const weight_entry_t s_weights[] = {
    { SYM_CHERRY, 50 },
    { SYM_LEMON,  50 },
    { SYM_ORANGE, 45 },
    { SYM_PLUM,   40 },
    { SYM_BELL,   30 },
    { SYM_BAR,    20 },
    { SYM_SEVEN,  12 },
    { SYM_STAR,    9 },
};

static slot_sym_t slot_random_sym(void)
{
    uint32_t r = esp_random() % 256;
    uint16_t acc = 0;
    for (int i = 0; i < (int)(sizeof(s_weights) / sizeof(s_weights[0])); i++) {
        acc += s_weights[i].weight;
        if (r < acc) return s_weights[i].sym;
    }
    return SYM_CHERRY;
}

/* ------------------------------------------------------------------ */
/* Paytable                                                            */
/* ------------------------------------------------------------------ */

typedef struct { slot_sym_t sym; uint32_t payout; } paytable_entry_t;

static const paytable_entry_t s_paytable[] = {
    { SYM_CHERRY,  5 },
    { SYM_LEMON,   8 },
    { SYM_ORANGE, 12 },
    { SYM_PLUM,   20 },
    { SYM_BELL,   50 },
    { SYM_BAR,   100 },
    { SYM_SEVEN, 250 },
    { SYM_STAR,  500 },
};

static uint32_t slot_payout(slot_sym_t a, slot_sym_t b, slot_sym_t c)
{
    if (a == b && b == c) {
        for (int i = 0; i < (int)(sizeof(s_paytable) / sizeof(s_paytable[0])); i++) {
            if (s_paytable[i].sym == a) return s_paytable[i].payout;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Bet levels                                                          */
/* ------------------------------------------------------------------ */

static const uint32_t s_bet_levels[] = { 1, 5, 10, 25, 50 };
#define NUM_BET_LEVELS (sizeof(s_bet_levels) / sizeof(s_bet_levels[0]))

/* ------------------------------------------------------------------ */
/* State machine                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    ST_IDLE,
    ST_SPINNING,
    ST_STOPPING_0,
    ST_STOPPING_1,
    ST_STOPPING_2,
    ST_RESULT,
    ST_NO_CREDITS,
} slot_state_t;

#define REEL_COUNT     3
#define SPIN_DURATION_MS  1200
#define STOP_DELAY_MS      350
#define REEL_SPEED         14
#define REEL_CELL_H        52
#define SYMBOLS_ON_REEL   24
#define STOP_TARGET       (SYMBOLS_ON_REEL / 2)

typedef struct {
    slot_sym_t symbols[SYMBOLS_ON_REEL];
    int   offset;
    bool  stopping;
    bool  stopped;
} reel_t;

typedef struct {
    slot_state_t state;
    TickType_t   state_tick;
    uint32_t     credits;
    int          bet_idx;
    uint32_t     last_payout;
    reel_t       reels[REEL_COUNT];
    slot_sym_t   result[REEL_COUNT];
    uint32_t     total_spins;
    uint32_t     total_wagered;
    uint32_t     total_won;
    uint32_t     largest_payout;
    char         message[32];
} slot_ctx_t;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static const char *sym_name(slot_sym_t s) { return s_sym_info[s].name; }
static uint16_t    sym_color(slot_sym_t s) { return s_sym_info[s].color; }
static uint32_t    current_bet(slot_ctx_t *ctx) { return s_bet_levels[ctx->bet_idx]; }

static void set_message(slot_ctx_t *ctx, const char *msg)
{
    strncpy(ctx->message, msg, sizeof(ctx->message) - 1);
    ctx->message[sizeof(ctx->message) - 1] = 0;
}

static void fill_reel_strip(reel_t *r)
{
    for (int i = 0; i < SYMBOLS_ON_REEL; i++) {
        r->symbols[i] = slot_random_sym();
    }
}

/* ------------------------------------------------------------------ */
/* App lifecycle                                                       */
/* ------------------------------------------------------------------ */

static void slot_init(arpile_app_ctx_t *app)
{
    slot_ctx_t *ctx = calloc(1, sizeof(slot_ctx_t));
    if (!ctx) return;
    ctx->credits = 1000;
    ctx->bet_idx = 2;
    ctx->state = ST_IDLE;
    set_message(ctx, "PRESS SPIN");
    for (int i = 0; i < REEL_COUNT; i++) {
        fill_reel_strip(&ctx->reels[i]);
        ctx->reels[i].offset = 0;
        ctx->reels[i].stopped = true;
    }
    app->user = ctx;
}

static void slot_destroy(arpile_app_ctx_t *app)
{
    if (app->user) {
        free(app->user);
        app->user = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void slot_event(arpile_app_ctx_t *app, const arpile_input_event_t *ev)
{
    slot_ctx_t *ctx = app->user;
    if (!ctx) return;
    if (ev->type != ARPILE_IN_EVENT_KEY_DOWN) return;
    uint16_t key = ev->key.keycode;

    switch (ctx->state) {
    case ST_IDLE:
        if (key == ARPILE_KEY_SPACE || key == ARPILE_KEY_ENTER) {
            uint32_t bet = current_bet(ctx);
            if (ctx->credits < bet) {
                set_message(ctx, "NO CREDITS!");
                ctx->state = ST_NO_CREDITS;
                ctx->state_tick = xTaskGetTickCount();
                arpile_ui_win_redraw(app->win);
                return;
            }
            ctx->credits -= bet;
            ctx->total_wagered += bet;
            ctx->total_spins++;
            for (int i = 0; i < REEL_COUNT; i++) {
                ctx->result[i] = slot_random_sym();
            }
            for (int i = 0; i < REEL_COUNT; i++) {
                reel_t *r = &ctx->reels[i];
                fill_reel_strip(r);
                r->symbols[STOP_TARGET] = ctx->result[i];
                r->offset = 0;
                r->stopping = false;
                r->stopped = false;
            }
            ctx->state = ST_SPINNING;
            ctx->state_tick = xTaskGetTickCount();
            ctx->last_payout = 0;
            set_message(ctx, "");
            arpile_ui_win_redraw(app->win);
        } else if (key == ARPILE_KEY_LEFT) {
            if (ctx->bet_idx > 0) { ctx->bet_idx--; arpile_ui_win_redraw(app->win); }
        } else if (key == ARPILE_KEY_RIGHT) {
            if (ctx->bet_idx < (int)NUM_BET_LEVELS - 1) { ctx->bet_idx++; arpile_ui_win_redraw(app->win); }
        } else if (key == ARPILE_KEY_UP) {
            ctx->bet_idx = NUM_BET_LEVELS - 1; arpile_ui_win_redraw(app->win);
        } else if (key == ARPILE_KEY_DOWN) {
            ctx->bet_idx = 0; arpile_ui_win_redraw(app->win);
        } else if (key == ARPILE_KEY_ESCAPE) {
            arpile_app_close(app);
        }
        break;

    case ST_NO_CREDITS:
        if (key == ARPILE_KEY_SPACE || key == ARPILE_KEY_ENTER) {
            ctx->credits = 1000;
            ctx->state = ST_IDLE;
            set_message(ctx, "CREDITS RESET");
            arpile_ui_win_redraw(app->win);
        } else if (key == ARPILE_KEY_ESCAPE) {
            arpile_app_close(app);
        }
        break;

    case ST_RESULT:
        if (key == ARPILE_KEY_SPACE || key == ARPILE_KEY_ENTER) {
            ctx->state = ST_IDLE;
            set_message(ctx, "PRESS SPIN");
            arpile_ui_win_redraw(app->win);
        } else if (key == ARPILE_KEY_ESCAPE) {
            arpile_app_close(app);
        }
        break;

    default:
        if (key == ARPILE_KEY_ESCAPE) {
            arpile_app_close(app);
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Update — animation tick, called ~30ms from UI loop                  */
/* ------------------------------------------------------------------ */

static void slot_update(arpile_app_ctx_t *app)
{
    slot_ctx_t *ctx = app->user;
    if (!ctx) return;

    bool need_redraw = false;
    TickType_t now = xTaskGetTickCount();
    uint32_t elapsed_ms = (now - ctx->state_tick) * 1000 / configTICK_RATE_HZ;

    switch (ctx->state) {
    case ST_SPINNING: {
        for (int i = 0; i < REEL_COUNT; i++) {
            reel_t *r = &ctx->reels[i];
            if (!r->stopped) {
                r->offset += REEL_SPEED;
                if (r->offset >= SYMBOLS_ON_REEL * REEL_CELL_H)
                    r->offset -= SYMBOLS_ON_REEL * REEL_CELL_H;
            }
        }
        uint32_t stop_time = SPIN_DURATION_MS;
        for (int i = 0; i < REEL_COUNT; i++) {
            if (elapsed_ms >= stop_time && !ctx->reels[i].stopping && !ctx->reels[i].stopped) {
                ctx->reels[i].stopping = true;
                ctx->state = ST_STOPPING_0 + i;
                ctx->state_tick = now;
                break;
            }
            stop_time += STOP_DELAY_MS;
        }
        need_redraw = true;
        break;
    }

    case ST_STOPPING_0:
    case ST_STOPPING_1:
    case ST_STOPPING_2: {
        int ri = ctx->state - ST_STOPPING_0;
        reel_t *r = &ctx->reels[ri];

        for (int i = 0; i < REEL_COUNT; i++) {
            if (i != ri && !ctx->reels[i].stopped) {
                ctx->reels[i].offset += REEL_SPEED;
                if (ctx->reels[i].offset >= SYMBOLS_ON_REEL * REEL_CELL_H)
                    ctx->reels[i].offset -= SYMBOLS_ON_REEL * REEL_CELL_H;
            }
        }

        uint32_t stop_elapsed_ms = (now - ctx->state_tick) * 1000 / configTICK_RATE_HZ;
        uint32_t stop_dur = 400;
        if (stop_elapsed_ms >= stop_dur) {
            r->offset = STOP_TARGET * REEL_CELL_H;
            r->stopped = true;
            r->stopping = false;

            if (ri < REEL_COUNT - 1) {
                ctx->state = ST_STOPPING_0 + (ri + 1);
                ctx->state_tick = now;
            } else {
                uint32_t payout = slot_payout(ctx->result[0], ctx->result[1], ctx->result[2]);
                ctx->last_payout = payout;
                ctx->credits += payout;
                ctx->total_won += payout;
                if (payout > ctx->largest_payout) ctx->largest_payout = payout;

                if (payout >= 250)      set_message(ctx, "JACKPOT!");
                else if (payout >= 50)  { char b[32]; snprintf(b, sizeof(b), "BIG WIN +%lu", (unsigned long)payout); set_message(ctx, b); }
                else if (payout > 0)    { char b[32]; snprintf(b, sizeof(b), "WIN +%lu", (unsigned long)payout); set_message(ctx, b); }
                else                    set_message(ctx, "NO WIN");

                ctx->state = (ctx->credits == 0) ? ST_NO_CREDITS : ST_RESULT;
                ctx->state_tick = now;
            }
        } else {
            float t = (float)stop_elapsed_ms / (float)stop_dur;
            float speed = REEL_SPEED * (1.0f - t * t);
            if (speed < 1.0f) speed = 1.0f;
            r->offset += (int)speed;
            if (r->offset >= SYMBOLS_ON_REEL * REEL_CELL_H)
                r->offset -= SYMBOLS_ON_REEL * REEL_CELL_H;
        }
        need_redraw = true;
        break;
    }

    default:
        break;
    }

    if (need_redraw) {
        arpile_ui_win_redraw(app->win);
    }
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* Layout: left half = reels + controls, right half = paytable/stats   */
/* ------------------------------------------------------------------ */

static void draw_reel_cell(ili9488_t *lcd, int x, int y, int w, int h,
                           slot_sym_t sym, bool highlight)
{
    uint16_t bg = highlight ? 0x4208 : 0x10A4;
    ui_draw_fill_rect(lcd, &(ui_rect_t){(uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h}, bg);
    ui_draw_outline(lcd, &(ui_rect_t){(uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h},
                    highlight ? 0xFFE0 : 0x7BEF);
    const char *name = sym_name(sym);
    int tw = (int)strlen(name) * CHAR_W;
    int tx = x + (w - tw) / 2;
    int ty = y + (h - CHAR_H) / 2;
    ui_draw_text(lcd, (uint16_t)tx, (uint16_t)ty, name, sym_color(sym), bg);
}

static void slot_render(arpile_app_ctx_t *app, ui_win_t *win)
{
    slot_ctx_t *ctx = app->user;
    if (!ctx) return;
    ili9488_t *lcd = arpile_ui_get_lcd();
    if (!lcd) return;

    ui_rect_t c = win_client_rect(win);
    int cx = c.x, cy = c.y, cw = c.w, ch = c.h;

    int half = cw / 2;
    int lx = cx;           /* left half x */
    int rx = cx + half;    /* right half x */
    int lw = half - 4;
    int rw = cw - half - 4;

    /* ============== LEFT HALF ============== */
    int y = cy;

    /* Title */
    {
        const char *t = "SLOT MACHINE";
        int tw2 = (int)strlen(t) * CHAR_W;
        ui_draw_text(lcd, (uint16_t)(lx + (lw - tw2) / 2), (uint16_t)y, t, 0xFFE0, 0x0000);
    }
    y += CHAR_H + 4;

    /* Reels */
    int reel_w = (lw - 12) / REEL_COUNT;
    int reel_h = REEL_CELL_H;
    int reel_y = y;
    bool show_result = (ctx->state == ST_RESULT || ctx->state == ST_NO_CREDITS);

    for (int i = 0; i < REEL_COUNT; i++) {
        int rxp = lx + 4 + i * (reel_w + 2);
        reel_t *r = &ctx->reels[i];
        slot_sym_t ds;
        if (r->stopped || ctx->state == ST_IDLE || ctx->state == ST_NO_CREDITS) {
            ds = r->symbols[STOP_TARGET];
        } else {
            int idx = (r->offset / REEL_CELL_H) % SYMBOLS_ON_REEL;
            ds = r->symbols[idx];
        }
        draw_reel_cell(lcd, rxp, reel_y, reel_w, reel_h, ds, show_result);
    }
    y = reel_y + reel_h + 6;

    /* Message */
    if (ctx->message[0]) {
        uint16_t mc = 0xFFFF;
        if (ctx->last_payout >= 250) mc = 0xFFE0;
        else if (ctx->last_payout > 0) mc = 0x07E0;
        else if (ctx->state == ST_NO_CREDITS) mc = 0xF800;
        int mw = (int)strlen(ctx->message) * CHAR_W;
        ui_draw_text(lcd, (uint16_t)(lx + (lw - mw) / 2), (uint16_t)y, ctx->message, mc, 0x0000);
    }
    y += CHAR_H + 8;

    /* Credits + Bet */
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "CREDITS: %lu", (unsigned long)ctx->credits);
        ui_draw_text(lcd, (uint16_t)(lx + 4), (uint16_t)y, buf, 0x07FF, 0x0000);
        snprintf(buf, sizeof(buf), "BET: %lu", (unsigned long)current_bet(ctx));
        int bw = (int)strlen(buf) * CHAR_W;
        ui_draw_text(lcd, (uint16_t)(lx + lw - bw - 4), (uint16_t)y, buf, 0xFFE0, 0x0000);
    }
    y += CHAR_H + 6;

    /* Controls */
    if (ctx->state == ST_IDLE) {
        const char *lines[] = {
            "ENTER: SPIN",
            "L/R: ADJ BET",
            "U/D: MAX/MIN",
            "ESC: EXIT",
        };
        uint16_t colors[] = { 0x07E0, 0x7BEF, 0x7BEF, 0xF800 };
        for (int i = 0; i < 4; i++) {
            ui_draw_text(lcd, (uint16_t)(lx + 4), (uint16_t)y, lines[i], colors[i], 0x0000);
            y += CHAR_H + 2;
        }
    } else if (ctx->state == ST_NO_CREDITS) {
        ui_draw_text(lcd, (uint16_t)(lx + 4), (uint16_t)y, "ENTER: RESET", 0xFFE0, 0x0000);
        y += CHAR_H + 2;
        ui_draw_text(lcd, (uint16_t)(lx + 4), (uint16_t)y, "ESC: EXIT", 0xF800, 0x0000);
        y += CHAR_H + 2;
    } else if (ctx->state == ST_RESULT) {
        ui_draw_text(lcd, (uint16_t)(lx + 4), (uint16_t)y, "ENTER: CONTINUE", 0x7BEF, 0x0000);
        y += CHAR_H + 2;
        ui_draw_text(lcd, (uint16_t)(lx + 4), (uint16_t)y, "ESC: EXIT", 0xF800, 0x0000);
        y += CHAR_H + 2;
    }

    /* ============== RIGHT HALF ============== */
    y = cy;

    /* Divider */
    ui_draw_vline(lcd, (uint16_t)(rx - 2), (uint16_t)cy, (uint16_t)ch, 0x4208);

    /* Paytable header */
    {
        const char *h = "PAYTABLE";
        int hw = (int)strlen(h) * CHAR_W;
        ui_draw_text(lcd, (uint16_t)(rx + (rw - hw) / 2), (uint16_t)y, h, 0xFFE0, 0x0000);
    }
    y += CHAR_H + 4;

    for (int i = 0; i < (int)(sizeof(s_paytable) / sizeof(s_paytable[0])); i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "3x %-6s %lu", sym_name(s_paytable[i].sym),
                 (unsigned long)s_paytable[i].payout);
        ui_draw_text(lcd, (uint16_t)(rx + 4), (uint16_t)y, buf,
                     sym_color(s_paytable[i].sym), 0x0000);
        y += CHAR_H + 2;
    }
    y += 6;

    /* Stats */
    {
        const char *sh = "STATS";
        ui_draw_text(lcd, (uint16_t)(rx + 4), (uint16_t)y, sh, 0x7BEF, 0x0000);
    }
    y += CHAR_H + 2;
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "SPINS: %lu", (unsigned long)ctx->total_spins);
        ui_draw_text(lcd, (uint16_t)(rx + 4), (uint16_t)y, buf, 0x7BEF, 0x0000);
        y += CHAR_H + 2;
        snprintf(buf, sizeof(buf), "WAGER: %lu", (unsigned long)ctx->total_wagered);
        ui_draw_text(lcd, (uint16_t)(rx + 4), (uint16_t)y, buf, 0x7BEF, 0x0000);
        y += CHAR_H + 2;
        snprintf(buf, sizeof(buf), "WON:    %lu", (unsigned long)ctx->total_won);
        ui_draw_text(lcd, (uint16_t)(rx + 4), (uint16_t)y, buf, 0x7BEF, 0x0000);
        y += CHAR_H + 2;
        snprintf(buf, sizeof(buf), "BEST:   %lu", (unsigned long)ctx->largest_payout);
        ui_draw_text(lcd, (uint16_t)(rx + 4), (uint16_t)y, buf, 0x7BEF, 0x0000);
    }
}

/* ------------------------------------------------------------------ */
/* App descriptor                                                      */
/* ------------------------------------------------------------------ */

static const arpile_app_ops_t slot_ops = {
    .init    = slot_init,
    .update  = slot_update,
    .event   = slot_event,
    .render  = slot_render,
    .destroy = slot_destroy,
};

const arpile_app_t arpile_app_slotsim = {
    .id   = "slotsim",
    .name = "Slot",
    .icon = "slotsim",
    .ops  = &slot_ops,
};
