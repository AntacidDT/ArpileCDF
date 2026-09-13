#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "arpile_ui.h"
#include "arpile_app.h"
#include "arpile_desktop_bg.h"
#include "arpile_icons.h"
#include "wifi_test.h"

extern const arpile_app_t arpile_app_terminal;
extern const arpile_app_t arpile_app_settings;
extern const arpile_app_t arpile_app_files_mgr;
extern const arpile_app_t arpile_app_editor;
extern const arpile_app_t arpile_app_math;
extern const arpile_app_t arpile_app_chem;
extern const arpile_app_t arpile_app_phys;
extern const arpile_app_t arpile_app_bio;
extern const arpile_app_t arpile_app_wiki;
extern const arpile_app_t arpile_app_vlc;
extern const arpile_app_t arpile_app_sysmon;
extern const arpile_app_t arpile_app_doom_app;
extern const arpile_app_t arpile_app_wifi;
extern const arpile_app_t arpile_app_voxel;
extern const arpile_app_t arpile_app_voxel;
extern const arpile_app_t arpile_app_prayertimes;
extern const arpile_app_t arpile_app_browser;
extern const arpile_app_t arpile_app_slotsim;
extern const arpile_app_t arpile_app_cowandwheat;

static ili9488_t *s_lcd = NULL;
static bool s_started = false;

/* --- window z-list: intrusive linked list, head = topmost --- */
static ui_win_t *s_head = NULL;
static uint8_t s_next_id = 1;
static ui_win_t s_windows[UI_MAX_WINDOWS];

/* --- dirty region (union of changed rects; repaint only this) --- */
static ui_rect_t s_dirty;
static bool s_dirty_valid = false;

/* --- shell state --- */
static bool s_launcher_open = false;
static int  s_launcher_scroll = 0;   /* row offset into grid */
static int  s_launcher_cols = 0;
static int  s_launcher_total_rows = 0;
static int  s_launcher_view_rows = 0;

/* --- pointer --- */
static int s_px = 0, s_py = 0;
static int s_cursor_x = -1, s_cursor_y = -1;  /* Last drawn cursor position */

/* Touchpad sensitivity multiplier (F8 = slower, F9 = faster). */
#define ARPILE_SENS_MIN 0.25f
#define ARPILE_SENS_MAX 6.0f
static float s_mouse_sens = 1.0f;

/* --- clock --- */
static TickType_t s_clock_tick_at = 0;
static uint32_t s_clock_sec = 0;

/* Wi-Fi panel icon position (must match draw_panel_clock). */
static void wifi_icon_rect(ui_rect_t *out)
{
    char cb[8];
    int hh = (int)(s_clock_sec / 3600) % 24;
    int mm = (int)(s_clock_sec % 3600) / 60;
    snprintf(cb, sizeof(cb), "%02d:%02d", hh, mm);
    uint16_t cw = ui_text_width(cb);
    int icon_w = 16;
    int iw = (int)UI_W - (int)cw - 6 - icon_w - 8;
    out->x = (uint16_t)iw;
    out->y = (uint16_t)(UI_H - UI_PANEL_H + 2);
    out->w = (uint16_t)icon_w;
    out->h = (uint16_t)(UI_PANEL_H - 4);
}

/* Simple 16x16 arrow cursor - drawn as a few rectangles for speed */
#define CURSOR_W 16
#define CURSOR_H 16

#define UI_WIN_TITLE_H 14
#define UI_WIN_EDGE    1
#define UICLOSE_W 8
#define UICLOSE_H 10
#define UIMAX_W 10
#define UIMAX_H 10

/* ------------------------------------------------------------------ */
/* Rendering helpers                                                   */
/* ------------------------------------------------------------------ */
static void push_dirty(const ui_rect_t *r)
{
    if (!s_dirty_valid) {
        s_dirty = *r;
        s_dirty_valid = true;
        return;
    }
    int16_t x0 = s_dirty.x < r->x ? (int16_t)s_dirty.x : (int16_t)r->x;
    int16_t y0 = s_dirty.y < r->y ? (int16_t)s_dirty.y : (int16_t)r->y;
    int16_t x1 = ((int16_t)s_dirty.x + s_dirty.w) > ((int16_t)r->x + r->w)
                     ? ((int16_t)s_dirty.x + s_dirty.w) : ((int16_t)r->x + r->w);
    int16_t y1 = ((int16_t)s_dirty.y + s_dirty.h) > ((int16_t)r->y + r->h)
                     ? ((int16_t)s_dirty.y + s_dirty.h) : ((int16_t)r->y + r->h);
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    s_dirty.x = (uint16_t)x0; s_dirty.y = (uint16_t)y0;
    s_dirty.w = (uint16_t)(x1 - x0); s_dirty.h = (uint16_t)(y1 - y0);
}

static void push_dirty_full(void)
{
    push_dirty(&(ui_rect_t){ 0, 0, UI_W, UI_H });
}

/* ------------------------------------------------------------------ */
/* Mouse cursor                                                        */
/* ------------------------------------------------------------------ */
static void draw_cursor_at(int x, int y)
{
    if (x < 0 || y < 0 || x >= UI_W || y >= UI_H) return;

    /* Classic Windows/KDE "arrow" pointer, 16x16, 1:1, 2 bytes per row (16 bits LSB-first). */
    static const uint16_t shape[16] = {
        0x0001, 0x0003, 0x0007, 0x000F,
        0x001F, 0x003F, 0x007F, 0x00FF,
        0x01FF, 0x03FF, 0x01C7, 0x0183,
        0x0301, 0x0600, 0x0C00, 0x1E00
    };

    for (int row = 0; row < 16; row++) {
        uint16_t bits = shape[row];
        if (y + row >= UI_H) break;
        for (int col = 0; col < 16; col++) {
            if ((bits >> col) & 1) {
                int px = x + col, py = y + row;
                if (px < UI_W) {
                    ui_draw_fill_rect(s_lcd, &(ui_rect_t){ (uint16_t)px, (uint16_t)py, 1, 1 }, 0xFFFF);
                }
            }
        }
    }
}

static void update_cursor_position(int x, int y)
{
    /* Clamp to display bounds */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= UI_W) x = UI_W - 1;
    if (y >= UI_H) y = UI_H - 1;

    /* Invalidate old position if moved */
    if (s_cursor_x != x || s_cursor_y != y) {
        if (s_cursor_x >= 0 && s_cursor_y >= 0) {
            ui_rect_t old = { (uint16_t)s_cursor_x, (uint16_t)s_cursor_y, CURSOR_W, CURSOR_H };
            if (!s_dirty_valid) {
                s_dirty = old;
                s_dirty_valid = true;
            } else {
                int16_t x0 = s_dirty.x < old.x ? (int16_t)s_dirty.x : (int16_t)old.x;
                int16_t y0 = s_dirty.y < old.y ? (int16_t)s_dirty.y : (int16_t)old.y;
                int16_t x1 = ((int16_t)s_dirty.x + s_dirty.w) > ((int16_t)old.x + old.w)
                                 ? ((int16_t)s_dirty.x + s_dirty.w) : ((int16_t)old.x + old.w);
                int16_t y1 = ((int16_t)s_dirty.y + s_dirty.h) > ((int16_t)old.y + old.h)
                                 ? ((int16_t)s_dirty.y + s_dirty.h) : ((int16_t)old.y + old.h);
                x0 = x0 < 0 ? 0 : x0;
                y0 = y0 < 0 ? 0 : y0;
                s_dirty.x = (uint16_t)x0; s_dirty.y = (uint16_t)y0;
                s_dirty.w = (uint16_t)(x1 - x0); s_dirty.h = (uint16_t)(y1 - y0);
            }
        }
        s_cursor_x = x;
        s_cursor_y = y;
        /* Dirty new position */
        ui_rect_t nw = { (uint16_t)x, (uint16_t)y, CURSOR_W, CURSOR_H };
        if (!s_dirty_valid) {
            s_dirty = nw;
            s_dirty_valid = true;
        } else {
            int16_t x0 = s_dirty.x < nw.x ? (int16_t)s_dirty.x : (int16_t)nw.x;
            int16_t y0 = s_dirty.y < nw.y ? (int16_t)s_dirty.y : (int16_t)nw.y;
            int16_t x1 = ((int16_t)s_dirty.x + s_dirty.w) > ((int16_t)nw.x + nw.w)
                             ? ((int16_t)s_dirty.x + s_dirty.w) : ((int16_t)nw.x + nw.w);
            int16_t y1 = ((int16_t)s_dirty.y + s_dirty.h) > ((int16_t)nw.y + nw.h)
                             ? ((int16_t)s_dirty.y + s_dirty.h) : ((int16_t)nw.y + nw.h);
            x0 = x0 < 0 ? 0 : x0;
            y0 = y0 < 0 ? 0 : y0;
            s_dirty.x = (uint16_t)x0; s_dirty.y = (uint16_t)y0;
            s_dirty.w = (uint16_t)(x1 - x0); s_dirty.h = (uint16_t)(y1 - y0);
        }
    }
    s_px = x;
    s_py = y;
}

static void raise_win(ui_win_t *win)
{
    if (s_head == win) {
        return;
    }
    if (win->prev) win->prev->next = win->next;
    if (win->next) win->next->prev = win->prev;
    win->prev = NULL;
    win->next = s_head;
    if (s_head) s_head->prev = win;
    s_head = win;
    push_dirty(&(ui_rect_t){ win->state.rect.x,
                             (uint16_t)(win->state.rect.y - UI_WIN_TITLE_H),
                             win->state.rect.w,
                             (uint16_t)(win->state.rect.h + UI_WIN_TITLE_H) });
}

static ui_win_t *top_win_at(int x, int y)
{
    for (ui_win_t *w = s_head; w; w = w->next) {
        if (!w->state.visible || w->state.minimized) {
            continue;
        }
        ui_rect_t dec = { w->state.rect.x, (uint16_t)(w->state.rect.y - UI_WIN_TITLE_H),
                          w->state.rect.w, (uint16_t)(w->state.rect.h + UI_WIN_TITLE_H) };
        if (ui_rect_contains(&dec, x, y)) {
            return w;
        }
    }
    return NULL;
}

ui_rect_t win_client_rect(const ui_win_t *w)
{
    ui_rect_t r;
    r.x = (uint16_t)((int)w->state.rect.x + UI_WIN_EDGE);
    r.y = (uint16_t)((int)w->state.rect.y + UI_WIN_EDGE);
    r.w = (uint16_t)(w->state.rect.w - 2 * UI_WIN_EDGE);
    r.h = (uint16_t)(w->state.rect.h - 2 * UI_WIN_EDGE);
    return r;
}

/* Draw a window (honors compositor clip). */
static void draw_window(ui_win_t *w)
{
    if (!w->state.visible || w->state.minimized) {
        return;
    }
    uint16_t title_col = w->state.focused ? UI_C_WIN_TITLE : UI_C_WIN_TITLE_I;
    uint16_t x0 = w->state.rect.x;
    uint16_t y0 = w->state.rect.y;
    uint16_t x1 = (uint16_t)(x0 + w->state.rect.w);

    /* --- title bar --- */
    ui_draw_fill_rect(s_lcd, &(ui_rect_t){ x0, (uint16_t)(y0 - UI_WIN_TITLE_H),
                                           w->state.rect.w, UI_WIN_TITLE_H }, title_col);
    /* active window: accent underline */
    if (w->state.focused) {
        ui_draw_hline(s_lcd, x0, (uint16_t)(y0 - 1), w->state.rect.w, UI_C_ACCENT);
    }
    /* maximize + close buttons (top-right) */
    uint16_t bx_max = (uint16_t)(x1 - UICLOSE_W - 2 - UIMAX_W);
    ui_rect_t bmax = { bx_max, (uint16_t)(y0 - UI_WIN_TITLE_H + 2), UIMAX_W, UIMAX_H };
    ui_draw_fill_rect(s_lcd, &bmax, title_col);
    ui_draw_outline(s_lcd, &bmax, UI_C_TEXT_LIGHT);
    if (w->state.maximized) {
        /* restore glyph: small square inset */
        ui_draw_fill_rect(s_lcd, &(ui_rect_t){ (uint16_t)(bmax.x + 2), (uint16_t)(bmax.y + 2),
                                                (uint16_t)(UIMAX_W - 4), (uint16_t)(UIMAX_H - 4) },
                           UI_C_TEXT_LIGHT);
    } else {
        ui_draw_outline(s_lcd, &(ui_rect_t){ (uint16_t)(bmax.x + 2), (uint16_t)(bmax.y + 2),
                                              (uint16_t)(UIMAX_W - 4), (uint16_t)(UIMAX_H - 4) },
                         UI_C_TEXT_LIGHT);
    }

    uint16_t bx = (uint16_t)(x1 - UICLOSE_W);
    ui_rect_t bclose = { bx, (uint16_t)(y0 - UI_WIN_TITLE_H + 2), UICLOSE_W, UICLOSE_H };
    ui_draw_fill_rect(s_lcd, &bclose, title_col);
    ui_draw_outline(s_lcd, &bclose, UI_C_TEXT_LIGHT);
    ui_draw_hline(s_lcd, (uint16_t)(bclose.x + 2), (uint16_t)(bclose.y + bclose.h - 3), 6, UI_C_TEXT_LIGHT);

    /* title (truncated so it never reaches the buttons) */
    int tmax = (int)bx_max - (int)x0 - 8;
    if (tmax < 0) tmax = 0;
    int tchars = tmax / CHAR_W;
    char tbuf[UI_WIN_TITLE_LEN + 1];
    int tl = strlen(w->state.title);
    if (tl > tchars && tchars > 0) {
        int n = tchars > 1 ? tchars - 1 : tchars;
        memcpy(tbuf, w->state.title, n); tbuf[n] = 0;
    } else {
        strncpy(tbuf, w->state.title, UI_WIN_TITLE_LEN);
        tbuf[UI_WIN_TITLE_LEN] = 0;
    }
    ui_draw_text(s_lcd, (uint16_t)(x0 + 4), (uint16_t)(y0 - UI_WIN_TITLE_H + 2),
                 tbuf, UI_C_TEXT_LIGHT, title_col);

    /* --- content --- */
    ui_rect_t c = win_client_rect(w);
    ui_draw_fill_rect(s_lcd, &c, UI_C_WIN_BG);
    if (w->ops.on_paint) {
        w->ops.on_paint(w);
    }
    ui_draw_outline(s_lcd, &c, UI_C_BORDER);
}

/* ------------------------------------------------------------------ */
/* Desktop shell                                                       */
/* ------------------------------------------------------------------ */
static void win_toggle_maximize(ui_win_t *w)
{
    if (!w) return;
    if (!w->state.maximized) {
        w->state.normal_rect = w->state.rect;
        w->state.rect = ui_rect(0, UI_WIN_TITLE_H, UI_W,
                                (uint16_t)(UI_H - UI_PANEL_H - UI_WIN_TITLE_H));
        w->state.maximized = true;
    } else {
        w->state.rect = w->state.normal_rect;
        w->state.maximized = false;
    }
    push_dirty_full();
}
static void draw_desktop_background(void)
{
    const ui_rect_t *clip = ui_get_compositor_clip();
    ui_rect_t dst = { 0, 0, UI_W, (uint16_t)(UI_H - UI_PANEL_H) };
    if (clip) {
        ui_rect_t c;
        if (!ui_rect_clip(&dst, clip, &c)) {
            return;
        }
        dst = c;
    }
    /* Sample the full-res desktop image into the destination rect. */
    ui_fb_blit(arpile_desktop_bg,
               dst.x, dst.y, dst.w, dst.h,
               dst.x, dst.y, dst.w, dst.h,
               DESKTOP_BG_W);
}

static void draw_panel_clock(void)
{
    char cb[8];
    int hh = (int)(s_clock_sec / 3600) % 24;
    int mm = (int)(s_clock_sec % 3600) / 60;
    snprintf(cb, sizeof(cb), "%02d:%02d", hh, mm);
    uint16_t cw = ui_text_width(cb);
    int x = (int)(UI_W - cw - 6);
    /* clear previous clock area a bit wider */
    ui_draw_fill_rect(s_lcd, &(ui_rect_t){ (uint16_t)(x - 4), (uint16_t)(UI_H - UI_PANEL_H + 2),
                                       (uint16_t)(cw + 8), (uint16_t)(UI_PANEL_H - 4) }, UI_C_PANEL);
    ui_draw_text(s_lcd, (uint16_t)x, (uint16_t)(UI_H - UI_PANEL_H + 5),
                 cb, UI_C_PANEL_TEXT, UI_C_PANEL);

    /* Wi-Fi status icon just left of the clock. */
    int icon_w = 16;
    int iw = UI_W - cw - 6 - icon_w - 8;
    /* draw Wi-Fi icon */
    ui_draw_icon_img(s_lcd, "wifi_task", (uint16_t)iw,
                     (uint16_t)(UI_H - UI_PANEL_H + 2), (uint16_t)icon_w);
    /* if not connected, draw a thin red vertical line through the icon */
    if (!arpile_wifi_is_connected()) {
        ui_draw_vline(s_lcd, (uint16_t)iw + 1, (uint16_t)(UI_H - UI_PANEL_H + 2),
                      (uint16_t)icon_w - 2, UI_C_RED);
    }
}

static void draw_panel(void)
{
    ui_rect_t panel = { 0, (uint16_t)(UI_H - UI_PANEL_H), UI_W, UI_PANEL_H };
    ui_draw_fill_rect(s_lcd, &panel, UI_C_PANEL);
    ui_draw_hline(s_lcd, 0, (uint16_t)(UI_H - UI_PANEL_H), UI_W, UI_C_PANEL_HI);

    /* launcher button */
    ui_rect_t launcher = { 2, (uint16_t)(UI_H - UI_PANEL_H + 3), 22, (uint16_t)(UI_PANEL_H - 6) };
    ui_draw_fill_rect(s_lcd, &launcher, s_launcher_open ? UI_C_ACCENT : UI_C_BUTTON);
    ui_draw_outline(s_lcd, &launcher, UI_C_BORDER);
    ui_draw_text(s_lcd, 8, (uint16_t)(UI_H - UI_PANEL_H + 6), "A",
                 s_launcher_open ? UI_C_TEXT_LIGHT : UI_C_PANEL_TEXT,
                 s_launcher_open ? UI_C_ACCENT : UI_C_BUTTON);

    /* task list */
    int tx = 28;
    for (ui_win_t *w = s_head; w; w = w->next) {
        if (!w->state.visible) continue;
        uint16_t label_w = 8 + ui_text_width(w->state.title) + 6;
        uint16_t col = w->state.focused ? UI_C_TASK_ACT : UI_C_BUTTON;
        ui_rect_t tb = { (uint16_t)tx, (uint16_t)(UI_H - UI_PANEL_H + 3),
                         label_w, (uint16_t)(UI_PANEL_H - 6) };
        ui_draw_fill_rect(s_lcd, &tb, col);
        ui_draw_outline(s_lcd, &tb, UI_C_BORDER);
        ui_draw_text(s_lcd, (uint16_t)(tx + 4), (uint16_t)(UI_H - UI_PANEL_H + 6),
                     w->state.title,
                     w->state.focused ? UI_C_TEXT_LIGHT : UI_C_PANEL_TEXT, col);
        tx += label_w + 3;
        if (tx > UI_W - 80) break;
    }

    draw_panel_clock();
}

/* Launcher: grid of app icons + labels. */
/* Launcher grid geometry, shared by drawing and click hit-testing so the
 * two can never drift apart. */
#define LAUNCH_TILE   64    /* icon square side */
#define LAUNCH_SP     20    /* horizontal gap between tiles */
#define LAUNCH_LABEL  12    /* label strip height under the tile */
#define LAUNCH_ROWH   (LAUNCH_TILE + LAUNCH_LABEL + 10)
#define LAUNCH_COLS   4

static void launcher_grid(const ui_rect_t *box, int app_count,
                          int *cols, int *gx, int *gy, int *view_rows,
                          int *total_rows)
{
    *cols = app_count < LAUNCH_COLS ? app_count : LAUNCH_COLS;
    if (*cols < 1) *cols = 1;
    *total_rows = (app_count + *cols - 1) / *cols;
    const int head_h = 26;
    int content_h = (int)box->h - head_h;
    *view_rows = content_h / LAUNCH_ROWH;
    if (*view_rows < 1) *view_rows = 1;
    int grid_w = *cols * LAUNCH_TILE + (*cols - 1) * LAUNCH_SP;
    *gx = box->x + (((int)box->w - grid_w) / 2);
    *gy = box->y + head_h;
}

/* Draw a label centered under a tile, truncated with ".." if too wide so
 * neighbouring labels can never overlap. */
static void launcher_label(int cx_center, int y, const char *name)
{
    char buf[26];
    size_t len = strlen(name);
    if (len > sizeof(buf) - 3) len = sizeof(buf) - 3;

    const int max_w = LAUNCH_TILE + LAUNCH_SP - 4;
    size_t fit = len;
    while (fit > 0) {
        memcpy(buf, name, fit);
        buf[fit] = 0;
        int w = ui_text_width(buf);
        if (fit < len) w += ui_text_width("..");
        if (w <= max_w) {
            break;
        }
        fit--;
    }
    buf[fit] = 0;
    if (fit < len) {
        strcat(buf, "..");
    } else {
        strcpy(buf, name);
    }

    uint16_t nw = ui_text_width(buf);
    ui_draw_text(s_lcd, (uint16_t)(cx_center - nw / 2), (uint16_t)y,
                 buf, UI_C_TEXT_LIGHT, UI_C_WIN_TITLE);
}

static void draw_launcher(void)
{
    ui_rect_t box = { 12, 18, (uint16_t)(UI_W - 24), (uint16_t)(UI_H - UI_PANEL_H - 28) };
    ui_draw_fill_rect(s_lcd, &box, UI_C_WIN_TITLE);
    ui_draw_outline(s_lcd, &box, UI_C_ACCENT);
    ui_draw_text(s_lcd, (uint16_t)(box.x + 6), (uint16_t)(box.y + 4),
                 "Applications", UI_C_TEXT_LIGHT, UI_C_WIN_TITLE);

    int app_count;
    const arpile_app_t **apps = arpile_app_list(&app_count);
    if (app_count <= 0) {
        ui_draw_text(s_lcd, (uint16_t)(box.x + 20), (uint16_t)(box.y + 40),
                     "No applications installed", UI_C_TEXT_LIGHT, UI_C_WIN_TITLE);
        return;
    }

    int cols, gx, gy, view_rows, total_rows;
    launcher_grid(&box, app_count, &cols, &gx, &gy, &view_rows, &total_rows);
    s_launcher_cols = cols;
    s_launcher_total_rows = total_rows;
    s_launcher_view_rows = view_rows;

    int max_scroll = total_rows - view_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (s_launcher_scroll > max_scroll) s_launcher_scroll = max_scroll;
    if (s_launcher_scroll < 0) s_launcher_scroll = 0;

    for (int i = 0; i < app_count; i++) {
        int r = i / cols, c = i % cols;
        int ry = r - s_launcher_scroll;
        if (ry < 0 || ry >= view_rows) {
            continue;               /* off the visible area (clipped anyway) */
        }
        int tx = gx + c * (LAUNCH_TILE + LAUNCH_SP);
        int ty = gy + ry * LAUNCH_ROWH;
        ui_rect_t tile = { (uint16_t)tx, (uint16_t)ty, LAUNCH_TILE, LAUNCH_TILE };
        ui_draw_fill_rect(s_lcd, &tile, UI_C_WIN_BG);
        ui_draw_outline(s_lcd, &tile, UI_C_BORDER);
        if (apps[i]->icon) {
            /* color logo if available, else scaled 1-bit fallback */
            uint8_t scale = (uint8_t)(LAUNCH_TILE / ARPILE_ICON_SIZE);
            int siz = ARPILE_ICON_SIZE * scale;
            if (!ui_draw_icon_img(s_lcd, apps[i]->icon,
                                  (uint16_t)(tx + ((int)LAUNCH_TILE - siz) / 2),
                                  (uint16_t)(ty + ((int)LAUNCH_TILE - siz) / 2),
                                  (uint16_t)siz)) {
                ui_draw_icon_scaled(s_lcd, apps[i]->icon,
                                    (uint16_t)(tx + ((int)LAUNCH_TILE - siz) / 2),
                                    (uint16_t)(ty + ((int)LAUNCH_TILE - siz) / 2),
                                    scale, UI_C_ACCENT, UI_C_WIN_BG);
            }
        }
        launcher_label(tx + LAUNCH_TILE / 2, ty + LAUNCH_TILE + 4,
                       apps[i]->name);
    }

    /* Scroll indicator when content overflows. */
    if (total_rows > view_rows) {
        int total = (int)box.h - 26 - 2;
        int th = total / total_rows;
        int tfy = box.y + 26 + s_launcher_scroll * th;
        if (th < 6) th = 6;
        ui_rect_t track = { (uint16_t)(box.x + box.w - 4),
                            (uint16_t)(box.y + 26), 2,
                            (uint16_t)(total - 2) };
        ui_draw_fill_rect(s_lcd, &track, UI_C_WIN_TITLE_I);
        ui_rect_t thumb = { track.x, (uint16_t)tfy, 2, (uint16_t)th };
        ui_draw_fill_rect(s_lcd, &thumb, UI_C_ACCENT);
    }
}

/* ------------------------------------------------------------------ */
/* Compositor: repaint dirty region top-down.                          */
/* ------------------------------------------------------------------ */
static void composite(void)
{
    ui_rect_t dirty;
    int cx, cy;
    bool has_cursor;

    if (!s_dirty_valid) {
        return;
    }
    dirty = s_dirty;
    s_dirty_valid = false;
    cx = s_cursor_x;
    cy = s_cursor_y;
    has_cursor = (cx >= 0 && cy >= 0);

    ui_set_compositor_clip(&dirty);
    draw_desktop_background();
    for (ui_win_t *w = s_head; w; w = w->next) {
        draw_window(w);
    }
    draw_panel();
    if (s_launcher_open) {
        draw_launcher();
    }
    /* Draw cursor on top */
    if (has_cursor) {
        draw_cursor_at(cx, cy);
    }
    ui_set_compositor_clip(NULL);
    /* Push the whole dirty region to the LCD in one atomic blit. */
    ui_fb_flush(s_lcd, &dirty);
}

/* ------------------------------------------------------------------ */
/* Window manager public API                                           */
/* ------------------------------------------------------------------ */
ui_win_t *arpile_ui_win_new(const char *title, int x, int y, int w, int h,
                            const ui_win_ops_t *ops, void *user)
{
    /* Reuse a freed window slot. Closed windows are unlinked and marked
     * visible=false, so a slot with visible==false is available. Without this,
     * s_next_id grew forever and every new window past UI_MAX_WINDOWS silently
     * got NULL -> apps launched but no window ever appeared. */
    ui_win_t *win = NULL;
    for (int i = 0; i < UI_MAX_WINDOWS; i++) {
        if (!s_windows[i].state.visible) {
            win = &s_windows[i];
            break;
        }
    }
    if (!win) {
        return NULL;   /* all UI_MAX_WINDOWS windows are open simultaneously */
    }
    memset(win, 0, sizeof(ui_win_t));
    win->id = s_next_id++;
    strncpy(win->state.title, title, UI_WIN_TITLE_LEN - 1);
    win->state.title[UI_WIN_TITLE_LEN - 1] = 0;
    win->state.rect = ui_rect((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h);
    win->state.normal_rect = win->state.rect;
    win->state.maximized = false;
    win->state.visible = true;
    win->state.focused = false;
    win->ops = ops ? *ops : (ui_win_ops_t){ 0 };
    win->user = user;

    /* insert at front (top most) */
    raise_win(win);
    return win;
}

void arpile_ui_win_focus(ui_win_t *win)
{
    bool prev = win->state.focused;
    for (ui_win_t *w = s_head; w; w = w->next) {
        if (w->state.focused) {
            w->state.focused = false;
            if (w->ops.on_focus) {
                w->ops.on_focus(w, false);
            }
            push_dirty(&(ui_rect_t){ w->state.rect.x,
                                     (uint16_t)(w->state.rect.y - UI_WIN_TITLE_H),
                                     w->state.rect.w, UI_WIN_TITLE_H });
        }
    }
    win->state.focused = true;
    raise_win(win);
    if (!prev && win->ops.on_focus) {
        win->ops.on_focus(win, true);
    }
    push_dirty(&(ui_rect_t){ win->state.rect.x,
                             (uint16_t)(win->state.rect.y - UI_WIN_TITLE_H),
                             win->state.rect.w, UI_WIN_TITLE_H });
}

/* Unlink a window from the list and free its slot (no callbacks). Safe to call
 * from the UI task only. */
void arpile_ui_win_teardown(ui_win_t *win)
{
    if (!win) {
        return;
    }
    if (win->prev) win->prev->next = win->next;
    if (win->next) win->next->prev = win->prev;
    if (s_head == win) s_head = win->next;
    win->state.visible = false;
    push_dirty(&(ui_rect_t){ win->state.rect.x,
                             (uint16_t)(win->state.rect.y - UI_WIN_TITLE_H),
                             win->state.rect.w,
                             (uint16_t)(win->state.rect.h + UI_WIN_TITLE_H) });
    /* Repaint the taskbar row: closing a window shifts the remaining taskbar
     * buttons, so the whole panel strip must be redrawn or the closed app's
     * button lingers as a ghost until some unrelated repaint. */
    push_dirty(&(ui_rect_t){ 0, (uint16_t)(UI_H - UI_PANEL_H - 1), UI_W,
                             (uint16_t)(UI_PANEL_H + 1) });
}

void arpile_ui_win_close(ui_win_t *win)
{
    /* Closing an app window must not run app teardown (destroy) synchronously:
     * callers include the USB HID task (Alt+Fx keys, launcher/X clicks) whose
     * 4 KB stack would overflow on FATFS-backed destroy and kill all input.
     * Route through arpile_app_close(), which only *requests* the close; the
     * heavy teardown runs later on the UI task in arpile_app_poll(). */
    arpile_app_close_win(win);
}

void arpile_ui_win_move(ui_win_t *win, int x, int y)
{
    uint16_t ox = win->state.rect.x, oy = win->state.rect.y;
    win->state.rect.x = (uint16_t)x;
    win->state.rect.y = (uint16_t)y;
    ui_rect_t old = { ox, (uint16_t)(oy - UI_WIN_TITLE_H), win->state.rect.w,
                      (uint16_t)(win->state.rect.h + UI_WIN_TITLE_H) };
    ui_rect_t nw = { (uint16_t)x, (uint16_t)(y - UI_WIN_TITLE_H), win->state.rect.w,
                     (uint16_t)(win->state.rect.h + UI_WIN_TITLE_H) };
    push_dirty(&old);
    push_dirty(&nw);
}

void arpile_ui_win_resize(ui_win_t *win, int w, int h)
{
    uint16_t ow = win->state.rect.w, oh = win->state.rect.h;
    win->state.rect.w = (uint16_t)w;
    win->state.rect.h = (uint16_t)h;
    ui_rect_t old = { win->state.rect.x, (uint16_t)(win->state.rect.y - UI_WIN_TITLE_H),
                      ow, (uint16_t)(oh + UI_WIN_TITLE_H) };
    ui_rect_t nw = { win->state.rect.x, (uint16_t)(win->state.rect.y - UI_WIN_TITLE_H),
                     (uint16_t)w, (uint16_t)(h + UI_WIN_TITLE_H) };
    push_dirty(&old);
    push_dirty(&nw);
}

void arpile_ui_win_set_title(ui_win_t *win, const char *title)
{
    strncpy(win->state.title, title, UI_WIN_TITLE_LEN - 1);
    win->state.title[UI_WIN_TITLE_LEN - 1] = 0;
    push_dirty(&(ui_rect_t){ win->state.rect.x,
                             (uint16_t)(win->state.rect.y - UI_WIN_TITLE_H),
                             win->state.rect.w, UI_WIN_TITLE_H });
}

void arpile_ui_win_redraw(ui_win_t *win)
{
    push_dirty(&(ui_rect_t){ win->state.rect.x,
                             (uint16_t)(win->state.rect.y - UI_WIN_TITLE_H),
                             win->state.rect.w,
                             (uint16_t)(win->state.rect.h + UI_WIN_TITLE_H) });
}

void arpile_ui_launcher_open(void)
{
    s_launcher_open = true;
    s_launcher_scroll = 0;
    push_dirty_full();
}

void arpile_ui_launcher_close(void)
{
    s_launcher_open = false;
    push_dirty_full();
}

bool arpile_ui_launcher_is_open(void)
{
    return s_launcher_open;
}

void arpile_ui_update_clock(void)
{
    s_clock_sec = (uint32_t)((xTaskGetTickCount() - s_clock_tick_at) * 1000 / configTICK_RATE_HZ / 1000);
    push_dirty(&(ui_rect_t){ UI_W - 64, (uint16_t)(UI_H - UI_PANEL_H),
                             (uint16_t)(UI_W - (UI_W - 64)), UI_PANEL_H });
}

bool arpile_ui_is_started(void)
{
    return s_started;
}

ili9488_t *arpile_ui_get_lcd(void)
{
    return s_lcd;
}

/* ------------------------------------------------------------------ */
/* Input dispatch                                                      */
/* ------------------------------------------------------------------ */
static bool launcher_click_at(int x, int y);

static void handle_global_shortcuts(const arpile_input_event_t *ev)
{
    if (ev->type != ARPILE_IN_EVENT_KEY_DOWN) {
        return;
    }
    uint8_t m = ev->key.modifier;
    bool ctrl = (m & (ARPILE_MOD_LCTRL | ARPILE_MOD_RCTRL)) ? true : false;
    bool alt  = (m & (ARPILE_MOD_LALT | ARPILE_MOD_RALT)) ? true : false;
    if (alt && ev->key.keycode == 0x17 && !ctrl) {   /* Alt+T = terminal */
        if (arpile_app_launch("terminal")) {
            push_dirty_full();   /* ensure the new terminal renders immediately */
        }
        return;
    }
    if (alt && ev->key.keycode == 0x1A && !ctrl) {   /* Alt+W = wifi */
        if (arpile_app_launch("wifi")) {
            push_dirty_full();
        }
        return;
    }
    /* GUI key reported as a keycode (0xE3 left, 0xE7 right) acts like the Windows key */
    if (ev->key.keycode == 0xE3 || ev->key.keycode == 0xE7) {
        /* Debounce: some keyboards emit the GUI press twice (modifier bit +
         * key-array); ignore a re-toggle within 300ms so one press = one toggle. */
        static TickType_t last_gui_toggle = 0;
        TickType_t now = xTaskGetTickCount();
        if (now - last_gui_toggle < pdMS_TO_TICKS(300)) {
            return;
        }
        last_gui_toggle = now;
        s_launcher_open = !s_launcher_open;
        push_dirty_full();
        push_dirty(&(ui_rect_t){ 2, (uint16_t)(UI_H - UI_PANEL_H + 3), 22, (uint16_t)(UI_PANEL_H - 6) });
        return;
    }
    /* Backtick/grave (`) key: launch the app tile under the cursor. Mouse-free
     * alternative to left-click for opening apps (the left button can wedge if a
     * release is missed by the HID stack, so this keeps launching reliable). */
    if (ev->key.keycode == 0x35) {
        if (launcher_click_at(s_px, s_py)) {
            return;
        }
    }
    /* Ctrl+Esc toggles launcher */
    if (ctrl && ev->key.keycode == ARPILE_KEY_ESCAPE) {
        s_launcher_open = !s_launcher_open;
        push_dirty_full();
    }
    /* Scroll the launcher grid with PageUp / PageDown */
    if (s_launcher_open && (ev->key.keycode == 0x4B || ev->key.keycode == 0x4E)) {
        s_launcher_scroll += (ev->key.keycode == 0x4E) ? 1 : -1;
        push_dirty(&(ui_rect_t){ 12, 18, (uint16_t)(UI_W - 24), (uint16_t)(UI_H - UI_PANEL_H - 28) });
    }
    /* Mouse sensitivity: F8 slower, F9 faster. */
    if (ev->key.keycode == 0x41 || ev->key.keycode == 0x42) {
        float delta = (ev->key.keycode == 0x42) ? 0.5f : -0.5f;
        s_mouse_sens += delta;
        if (s_mouse_sens < ARPILE_SENS_MIN) s_mouse_sens = ARPILE_SENS_MIN;
        if (s_mouse_sens > ARPILE_SENS_MAX) s_mouse_sens = ARPILE_SENS_MAX;
        printf("[UI] mouse sensitivity x%0.2f\r\n", s_mouse_sens);
    }
    /* Alt+F1..F5: close the window at that taskbar position (front = top-most).
     * F1=0x3A, F2=0x3B, F3=0x3C, F4=0x3D, F5=0x3E. */
    if (alt && ev->key.keycode >= 0x3A && ev->key.keycode <= 0x3E) {
        int n = ev->key.keycode - 0x3A;   /* F1 -> 0 (front), F5 -> 4 */
        ui_win_t *w = s_head;
        for (int i = 0; i < n && w; i++) {
            w = w->next;
        }
        if (w) {
            arpile_ui_win_close(w);
        }
    }
}

/* held arrow key state for cursor repeat */
static bool s_key_up = false;
static bool s_key_down = false;
static bool s_key_left = false;
static bool s_key_right = false;

static void handle_key(const arpile_input_event_t *ev)
{
    handle_global_shortcuts(ev);

    /* Arrow keys normally move the mouse cursor; but if a focused window opts
     * in (text editor), forward them to the app so it can move its own cursor. */
    ui_win_t *focused = NULL;
    for (ui_win_t *w = s_head; w; w = w->next) {
        if (w->state.focused) {
            focused = w;
            break;
        }
    }
    if (ev->type == ARPILE_IN_EVENT_KEY_DOWN &&
        ev->key.keycode == ARPILE_KEY_F10 && focused) {
        win_toggle_maximize(focused);
        return;
    }
    bool nav_captured = focused && focused->state.capture_nav &&
                        (ev->key.keycode == ARPILE_KEY_UP ||
                         ev->key.keycode == ARPILE_KEY_DOWN ||
                         ev->key.keycode == ARPILE_KEY_LEFT ||
                         ev->key.keycode == ARPILE_KEY_RIGHT);
    if (nav_captured) {
        if (focused->ops.on_key) {
            focused->ops.on_key(focused, ev);
        }
        return;
    }

    /* Arrow keys move the cursor; track held state for auto-repeat */
    bool is_down = (ev->type == ARPILE_IN_EVENT_KEY_DOWN);
    bool moved = false;
    int nx = s_px, ny = s_py;
    switch (ev->key.keycode) {
    case ARPILE_KEY_UP:
        s_key_up = is_down;
        if (is_down) { ny -= 8; moved = true; }
        break;
    case ARPILE_KEY_DOWN:
        s_key_down = is_down;
        if (is_down) { ny += 8; moved = true; }
        break;
    case ARPILE_KEY_LEFT:
        s_key_left = is_down;
        if (is_down) { nx -= 8; moved = true; }
        break;
    case ARPILE_KEY_RIGHT:
        s_key_right = is_down;
        if (is_down) { nx += 8; moved = true; }
        break;
    default: break;
    }
    if (moved) {
        update_cursor_position(nx, ny);
        return;
    }

    if (focused && focused->ops.on_key) {
        focused->ops.on_key(focused, ev);
    }
}

/* mouse drag state */
static ui_win_t *s_drag_win = NULL;
static int s_drag_offx = 0, s_drag_offy = 0;

/* Launch the app tile under (x,y) while the launcher is open. Used by both the
 * left-click handler and the backtick (`) keyboard shortcut, so the same hit-test
 * drives mouse and mouse-free launching. Returns true if a tile was launched. */
static bool launcher_click_at(int x, int y)
{
    if (!s_launcher_open) {
        return false;
    }
    ui_rect_t box = { 12, 18, (uint16_t)(UI_W - 24), (uint16_t)(UI_H - UI_PANEL_H - 28) };
    if (!ui_rect_contains(&box, x, y)) {
        s_launcher_open = false;
        push_dirty_full();
        return false;
    }
    int app_count;
    const arpile_app_t **apps = arpile_app_list(&app_count);
    if (app_count > 0) {
        int cols, gx, gy, view_rows, total_rows;
        launcher_grid(&box, app_count, &cols, &gx, &gy, &view_rows, &total_rows);
        if (s_launcher_scroll > total_rows - view_rows)
            s_launcher_scroll = total_rows - view_rows;
        if (s_launcher_scroll < 0) s_launcher_scroll = 0;
        for (int i = 0; i < app_count; i++) {
            int r = i / cols, c = i % cols;
            int ry = r - s_launcher_scroll;
            if (ry < 0 || ry >= view_rows) continue;
            ui_rect_t tile = { (uint16_t)(gx + c * (LAUNCH_TILE + LAUNCH_SP)),
                               (uint16_t)(gy + ry * LAUNCH_ROWH),
                               LAUNCH_TILE, LAUNCH_TILE };
            if (ui_rect_contains(&tile, x, y)) {
                arpile_app_ctx_t *launched = arpile_app_launch(apps[i]->id);
                printf("[UI] tile click '%s' -> %s\r\n", apps[i]->id,
                       launched ? "opened" : "FAILED(NULL)");
                s_launcher_open = false;
                s_launcher_scroll = 0;
                push_dirty_full();
                return true;
            }
        }
    }
    return false;
}

static void handle_mouse(const arpile_input_event_t *ev)
{
    /* Apply relative displacement to current cursor position (deltas from driver) */
    if (ev->type == ARPILE_IN_EVENT_MOUSE_MOVE) {
        update_cursor_position(s_px + (int)(ev->mouse.x * s_mouse_sens),
                               s_py + (int)(ev->mouse.y * s_mouse_sens));
    }

    if (ev->type == ARPILE_IN_EVENT_MOUSE_WHEEL) {
        if (s_launcher_open) {
            s_launcher_scroll += (-ev->wheel);   /* wheel up = earlier rows */
            push_dirty(&(ui_rect_t){ 12, 18, (uint16_t)(UI_W - 24), (uint16_t)(UI_H - UI_PANEL_H - 28) });
            return;
        }
        /* Wheel scrolls the focused window (e.g. voxel hotbar slot selector).
         * The event union shares storage: never rewrite mouse.x/y of a wheel
         * event or it clobbers `wheel`. */
        for (ui_win_t *w = s_head; w; w = w->next) {
            if (w->state.focused && w->ops.on_mouse) {
                w->ops.on_mouse(w, ev);
                break;
            }
        }
        return;
    }

    if (ev->type == ARPILE_IN_EVENT_MOUSE_BTN) {
        bool pressed = (ev->mouse.buttons & ARPILE_MOUSE_BTN_LEFT) ? true : false;

        if (pressed) {
            /* Wi-Fi panel icon toggles the Wi-Fi app */
            ui_rect_t wrect;
            wifi_icon_rect(&wrect);
            if (ui_rect_contains(&wrect, s_px, s_py)) {
                if (arpile_app_launch("wifi")) {
                    push_dirty_full();
                }
                return;
            }

            /* launcher button */
            ui_rect_t launcher = { 2, (uint16_t)(UI_H - UI_PANEL_H + 3), 22, (uint16_t)(UI_PANEL_H - 6) };
            if (ui_rect_contains(&launcher, s_px, s_py)) {
                s_launcher_open = !s_launcher_open;
                if (s_launcher_open) s_launcher_scroll = 0;
                push_dirty_full();
                push_dirty(&launcher);
                return;
            }
            if (s_launcher_open) {
                launcher_click_at(s_px, s_py);
                return;   /* launcher consumes clicks while open */
            }

            /* taskbar buttons */
            int tx = 28;
            for (ui_win_t *w = s_head; w; w = w->next) {
                if (!w->state.visible) continue;
                uint16_t label_w = 8 + ui_text_width(w->state.title) + 6;
                ui_rect_t tb = { (uint16_t)tx, (uint16_t)(UI_H - UI_PANEL_H + 3),
                                 label_w, (uint16_t)(UI_PANEL_H - 6) };
                if (ui_rect_contains(&tb, s_px, s_py)) {
                    w->state.minimized = false;
                    arpile_ui_win_focus(w);
                    return;
                }
                tx += label_w + 3;
                if (tx > UI_W - 80) break;
            }

            /* window hit-test */
            ui_win_t *t = top_win_at(s_px, s_py);
            if (t) {
                arpile_ui_win_focus(t);
                uint16_t x1 = (uint16_t)(t->state.rect.x + t->state.rect.w);
                uint16_t y0 = t->state.rect.y;
                /* maximize/restore button (left of close) */
                ui_rect_t bmax = { (uint16_t)(x1 - UICLOSE_W - 2 - UIMAX_W),
                                   (uint16_t)(y0 - UI_WIN_TITLE_H + 2), UIMAX_W, UIMAX_H };
                if (ui_rect_contains(&bmax, s_px, s_py)) {
                    win_toggle_maximize(t);
                    return;
                }
                /* close button */
                ui_rect_t bc = { (uint16_t)(x1 - UICLOSE_W), (uint16_t)(y0 - UI_WIN_TITLE_H + 2),
                                 UICLOSE_W, UICLOSE_H };
                if (ui_rect_contains(&bc, s_px, s_py)) {
                    arpile_ui_win_close(t);
                    return;
                }
                /* title bar -> start drag (not while maximized) */
                if (!t->state.maximized &&
                    s_py < y0 && s_py >= (int)(y0 - UI_WIN_TITLE_H)) {
                    s_drag_win = t;
                    s_drag_offx = s_px - (int)t->state.rect.x;
                    s_drag_offy = s_py - (int)t->state.rect.y;
                    return;
                }
                /* content mouse */
                if (t->ops.on_mouse) {
                    arpile_input_event_t local = *ev;
                    local.mouse.x -= (int)t->state.rect.x;
                    local.mouse.y -= (int)t->state.rect.y;
                    t->ops.on_mouse(t, &local);
                }
            }
        } else {
            s_drag_win = NULL;
        }
    } else if (ev->type == ARPILE_IN_EVENT_MOUSE_MOVE) {
        if (s_drag_win) {
            int nx = s_px - s_drag_offx;
            int ny = s_py - s_drag_offy;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            /* keep window + title above panel */
            if (ny > (int)(UI_H - UI_PANEL_H - (int)s_drag_win->state.rect.h - UI_WIN_TITLE_H)) {
                ny = UI_H - UI_PANEL_H - (int)s_drag_win->state.rect.h - UI_WIN_TITLE_H;
            }
            arpile_ui_win_move(s_drag_win, nx, ny);
        } else {
            /* Mouse-look: while not dragging, forward raw deltas to the
             * focused window (e.g. the voxel first-person view). Deltas are
             * window-independent, so there is nothing to translate. */
            for (ui_win_t *w = s_head; w; w = w->next) {
                if (w->state.focused && w->ops.on_mouse) {
                    w->ops.on_mouse(w, ev);
                    break;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Event pump: HID -> UI                                               */
/* ------------------------------------------------------------------ */
static void on_hid_event(const arpile_input_event_t *ev)
{
    switch (ev->type) {
    case ARPILE_IN_EVENT_MOUSE_MOVE:
    case ARPILE_IN_EVENT_MOUSE_BTN:
    case ARPILE_IN_EVENT_MOUSE_WHEEL:
        handle_mouse(ev);
        break;
    case ARPILE_IN_EVENT_KEY_DOWN:
    case ARPILE_IN_EVENT_KEY_UP:
        handle_key(ev);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Main UI task                                                        */
/* ------------------------------------------------------------------ */
static void ui_task(void *arg)
{
    (void)arg;
    while (1) {
        /* Auto-repeat: hold arrow keys to keep moving the cursor */
        if (s_key_up || s_key_down || s_key_left || s_key_right) {
            int nx = s_px, ny = s_py;
            if (s_key_up)    ny -= 8;
            if (s_key_down)  ny += 8;
            if (s_key_left)  nx -= 8;
            if (s_key_right) nx += 8;
            update_cursor_position(nx, ny);
        }
        /* Poll running apps (e.g. wifi app refreshes on scan/state change) */
        arpile_app_poll();
        /* Drive Wi-Fi scan completion from the central loop so scans make
         * progress even when neither the WiFi app nor a terminal is open. */
        arpile_wifi_poll();
        composite();
        /* update clock every ~1s: repaint only the panel clock region */
        TickType_t now = xTaskGetTickCount();
        uint32_t sec = (uint32_t)((now - s_clock_tick_at) * 1000 / configTICK_RATE_HZ / 1000);
        if (sec != s_clock_sec) {
            s_clock_sec = sec;
            push_dirty(&(ui_rect_t){ UI_W - 64, (uint16_t)(UI_H - UI_PANEL_H),
                                     (uint16_t)(UI_W - (UI_W - 64)), UI_PANEL_H });
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* ------------------------------------------------------------------ */
/* Public lifecycle                                                    */
/* ------------------------------------------------------------------ */
esp_err_t arpile_ui_start(ili9488_t *lcd)
{
    if (s_started) {
        return ESP_OK;
    }
    s_lcd = lcd;
    s_head = NULL;
    s_next_id = 1;
    s_clock_tick_at = xTaskGetTickCount();
    s_clock_sec = 0;
    s_launcher_open = false;

    /* Register built-in applications */
    arpile_app_register(&arpile_app_terminal);
    arpile_app_register(&arpile_app_settings);
    arpile_app_register(&arpile_app_files_mgr);
    arpile_app_register(&arpile_app_wifi);
    arpile_app_register(&arpile_app_editor);
    arpile_app_register(&arpile_app_math);
    arpile_app_register(&arpile_app_chem);
    arpile_app_register(&arpile_app_phys);
    arpile_app_register(&arpile_app_bio);
    arpile_app_register(&arpile_app_wiki);
    arpile_app_register(&arpile_app_vlc);
    arpile_app_register(&arpile_app_sysmon);
    arpile_app_register(&arpile_app_doom_app);
    arpile_app_register(&arpile_app_voxel);
    arpile_app_register(&arpile_app_prayertimes);
    arpile_app_register(&arpile_app_slotsim);
    arpile_app_register(&arpile_app_cowandwheat);

    arpile_usb_host_set_event_cb(on_hid_event);

    if (xTaskCreatePinnedToCore(ui_task, "arpile_ui", 8192, NULL, 8, NULL, 0)
        != pdPASS) {
        return ESP_FAIL;
    }

    s_started = true;
    push_dirty_full();
    composite();   /* draw the desktop immediately */

    /* Boot straight into the voxel demo app (acceptance criterion). */
    arpile_app_launch("voxel");
    return ESP_OK;
}