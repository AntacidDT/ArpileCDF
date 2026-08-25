/* Arpile WiFi application: scan for networks, select one, enter password,
 * and connect. Shows status + signal strength. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "arpile_app.h"
#include "arpile_ui.h"
#include "wifi_test.h"

#define WIFI_UI_AP_ROWS 6
#define WIFI_UI_ROW_H   (CHAR_H + 6)
#define WIFI_UI_PW_MAX  63
#define WIFI_UI_SSID_MAX 33
#define WIFI_UI_MSG_LEN 64

typedef enum {
    WIFI_UI_VIEW_LIST = 0,    /* scan results list */
    WIFI_UI_VIEW_PASS,        /* password prompt for selected AP */
    WIFI_UI_VIEW_SSID,        /* manual connect: type SSID (Ctrl+I) */
    WIFI_UI_VIEW_SSID_PASS,   /* manual connect: SSID set, type password */
} wifi_ui_view_t;

typedef struct {
    wifi_ui_view_t view;
    int sel;                    /* selected AP row */
    int scroll;                 /* list scroll offset (rows) */
    char password[WIFI_UI_PW_MAX + 1];
    int pw_len;
    char ssid[WIFI_UI_SSID_MAX];
    int ssid_len;
    char msg[WIFI_UI_MSG_LEN];  /* status/error line */
    uint32_t last_paint_ms;     /* for throttling status refreshes */
} wifi_state_t;

static wifi_state_t *g_self = NULL;   /* global so panel icon can signal app */
static uint32_t s_scan_last_ms = 0;   /* ms of last scan kick-off */

/* Signal strength bars (0..4) from RSSI. */
static int rssi_bars(int8_t rssi)
{
    if (rssi >= -50) return 4;
    if (rssi >= -60) return 3;
    if (rssi >= -70) return 2;
    if (rssi >= -80) return 1;
    return 0;
}

static const char *auth_str(int8_t a)
{
    switch (a) {
    case 0:  return "Open";
    case 1:  return "WEP";
    case 2:  return "WPA";
    case 3:  return "WPA2";
    case 4:  return "WPA/WPA2";
    case 5:  return "Ent";
    case 6:  return "WPA3";
    case 7:  return "WPA2/3";
    default: return "?";
    }
}

static void do_scan(void)
{
    arpile_wifi_start_scan();
    s_scan_last_ms = (uint32_t)(xTaskGetTickCount() * 1000 / configTICK_RATE_HZ);
}

/* Absolute index of the highlighted row in the shared AP model. */
static int list_index(const wifi_state_t *st)
{
    return st->scroll + st->sel;
}

/* Highlight absolute row abs_idx, scrolling the window to keep it visible. */
static void list_set_index(wifi_state_t *st, int abs_idx)
{
    int count = arpile_wifi_get_ap_count();
    if (count <= 0) {
        st->sel = 0;
        st->scroll = 0;
        return;
    }
    if (abs_idx < 0) abs_idx = 0;
    if (abs_idx > count - 1) abs_idx = count - 1;
    if (abs_idx < st->scroll) {
        st->scroll = abs_idx;                       /* jumped above window */
    } else if (abs_idx > st->scroll + WIFI_UI_AP_ROWS - 1) {
        st->scroll = abs_idx - WIFI_UI_AP_ROWS + 1; /* jumped below window */
    }
    st->sel = abs_idx - st->scroll;
}

/* Enter on the selected network: open networks connect at once, secured ones
 * open the password prompt. */
static void do_choose(wifi_state_t *st)
{
    arpile_wifi_ap_t ap;
    if (!arpile_wifi_get_ap(list_index(st), &ap)) {
        snprintf(st->msg, sizeof(st->msg), "No network selected");
        return;
    }
    if (ap.authmode == 0) {
        esp_err_t r = arpile_wifi_connect(ap.ssid, "");
        snprintf(st->msg, sizeof(st->msg), r == ESP_OK ? "Connecting to %s..." : "Connect failed",
                 ap.ssid);
    } else {
        st->view = WIFI_UI_VIEW_PASS;
        st->pw_len = 0;
    }
}

static void do_connect(wifi_state_t *st)
{
    arpile_wifi_ap_t ap;
    if (!arpile_wifi_get_ap(st->sel + st->scroll, &ap)) {
        snprintf(st->msg, sizeof(st->msg), "No network selected");
        return;
    }
    if (ap.authmode == 0) {
        /* open network: connect with no password */
        esp_err_t r = arpile_wifi_connect(ap.ssid, "");
        snprintf(st->msg, sizeof(st->msg), r == ESP_OK ? "Connecting to %s..." : "Connect failed",
                 ap.ssid);
    } else {
        if (st->pw_len == 0) {
            snprintf(st->msg, sizeof(st->msg), "Enter a password for %s", ap.ssid);
            return;
        }
        st->password[st->pw_len] = 0;
        esp_err_t r = arpile_wifi_connect(ap.ssid, st->password);
        snprintf(st->msg, sizeof(st->msg), r == ESP_OK ? "Connecting to %s..." : "Connect failed",
                 ap.ssid);
    }
}

/* ---------------- render ---------------- */

static void paint_header(ili9488_t *lcd, const ui_rect_t *c)
{
    char line[WIFI_UI_MSG_LEN];
    arpile_wifi_state_t st = arpile_wifi_get_state();
    if (st == ARPILE_WIFI_CONNECTED) {
        snprintf(line, sizeof(line), "Connected to %s", arpile_wifi_get_connected_ssid());
    } else if (st == ARPILE_WIFI_CONNECTING) {
        snprintf(line, sizeof(line), "Connecting...");
    } else {
        snprintf(line, sizeof(line), "Not connected");
    }
    uint16_t tw = ui_text_width(line);
    ui_draw_text(lcd, (uint16_t)(c->x + c->w - tw - 4), (uint16_t)(c->y + 3),
                 line, UI_C_TEXT, UI_C_WIN_BG);
    /* "Scan" button */
    ui_rect_t b = { c->x + 4, c->y + 2, 48, CHAR_H + 4 };
    ui_draw_fill_rect(lcd, &b, UI_C_BUTTON);
    ui_draw_outline(lcd, &b, UI_C_BORDER);
    ui_draw_text(lcd, (uint16_t)(b.x + 5), (uint16_t)(b.y + 2), "Scan", UI_C_TEXT_LIGHT, UI_C_BUTTON);
}

static void paint_list(ili9488_t *lcd, const ui_rect_t *c, wifi_state_t *st)
{
    int rows = (c->h - CHAR_H - 12) / WIFI_UI_ROW_H;
    if (rows > WIFI_UI_AP_ROWS) rows = WIFI_UI_AP_ROWS;
    if (rows < 1) rows = 1;

    int count = arpile_wifi_get_ap_count();
    if (count == 0) {
        if (st->msg[0]) {
            ui_draw_text(lcd, (uint16_t)(c->x + 8), (uint16_t)(c->y + CHAR_H + 2),
                         st->msg, UI_C_TEXT_DIM, UI_C_WIN_BG);
        }
        ui_draw_text(lcd, (uint16_t)(c->x + 8), (uint16_t)(c->y + CHAR_H + 16),
                     arpile_wifi_scan_in_progress() ? "Scanning..." : "No networks found (auto-rescanning)",
                     UI_C_TEXT, UI_C_WIN_BG);
        return;
    }

    if (count > 0 && st->scroll > count - 1) {
        st->scroll = count - 1;
    }
    if (st->sel < 0) st->sel = 0;
    if (st->sel > WIFI_UI_AP_ROWS - 1) st->sel = WIFI_UI_AP_ROWS - 1;

    for (int i = 0; i < rows; i++) {
        int idx = st->scroll + i;
        if (idx >= count) break;
        arpile_wifi_ap_t ap;
        if (!arpile_wifi_get_ap(idx, &ap)) continue;
        int y = c->y + CHAR_H + 14 + i * WIFI_UI_ROW_H;

        if (i == st->sel) {
            ui_rect_t selr = { (uint16_t)(c->x + 1), (uint16_t)y - 2,
                               (uint16_t)(c->w - 2), (uint16_t)WIFI_UI_ROW_H };
            ui_draw_fill_rect(lcd, &selr, UI_C_ACCENT);
        }
        /* SSID */
        ui_draw_text(lcd, (uint16_t)(c->x + 8), (uint16_t)y,
                     ap.ssid, i == st->sel ? UI_C_TEXT_LIGHT : UI_C_TEXT,
                     i == st->sel ? UI_C_ACCENT : UI_C_WIN_BG);
        /* signal bars */
        int b = rssi_bars(ap.rssi);
        int bx = c->x + c->w - 70;
        for (int k = 0; k < 4; k++) {
            uint16_t h = (uint16_t)(4 + k * 3);
            ui_rect_t bar = { (uint16_t)(bx + k * 5), (uint16_t)(y + (uint16_t)((CHAR_H - h) / 2)),
                              3, h };
            ui_draw_fill_rect(lcd, &bar, k < b ? UI_C_ACCENT : UI_C_BORDER);
        }
        /* auth mode */
        char ab[8];
        snprintf(ab, sizeof(ab), "%s", auth_str(ap.authmode));
        ui_draw_text(lcd, (uint16_t)(c->x + c->w - 48), (uint16_t)y,
                     ab, i == st->sel ? UI_C_TEXT_LIGHT : UI_C_TEXT,
                     i == st->sel ? UI_C_ACCENT : UI_C_WIN_BG);
    }
}

static void paint_pass_view(ili9488_t *lcd, const ui_rect_t *c, wifi_state_t *st)
{
    if (st->view == WIFI_UI_VIEW_SSID) {
        char line[96];
        snprintf(line, sizeof(line), "SSID: %s_", st->ssid);
        ui_draw_text(lcd, (uint16_t)(c->x + 8), (uint16_t)(c->y + 8),
                     line, UI_C_TEXT, UI_C_WIN_BG);
        ui_rect_t f = { (uint16_t)(c->x + 8), (uint16_t)(c->y + CHAR_H + 18),
                        (uint16_t)(c->w - 16), (uint16_t)(CHAR_H + 8) };
        ui_draw_fill_rect(lcd, &f, UI_C_WIN_BG);
        ui_draw_outline(lcd, &f, UI_C_ACCENT);
        ui_draw_text(lcd, (uint16_t)(c->x + 16), (uint16_t)(c->y + CHAR_H + 30),
                     "Type network name, Enter for password, Esc to cancel",
                     UI_C_TEXT_DIM, UI_C_WIN_BG);
        return;
    }

    if (st->view == WIFI_UI_VIEW_SSID_PASS) {
        char line[96];
        snprintf(line, sizeof(line), "SSID: %s", st->ssid);
        ui_draw_text(lcd, (uint16_t)(c->x + 8), (uint16_t)(c->y + 8),
                     line, UI_C_TEXT, UI_C_WIN_BG);
        /* password field */
        ui_rect_t pw = { (uint16_t)(c->x + 8), (uint16_t)(c->y + CHAR_H + 18),
                         (uint16_t)(c->w - 16), (uint16_t)(CHAR_H + 8) };
        ui_draw_fill_rect(lcd, &pw, UI_C_WIN_BG);
        ui_draw_outline(lcd, &pw, UI_C_ACCENT);
        char masked[WIFI_UI_PW_MAX + 1];
        int mlen = st->pw_len;
        if (mlen > (int)sizeof(masked) - 1) mlen = (int)sizeof(masked) - 1;
        for (int i = 0; i < mlen; i++) masked[i] = '*';
        masked[mlen] = 0;
        ui_draw_text(lcd, (uint16_t)(pw.x + 4), (uint16_t)(pw.y + 4),
                     masked, UI_C_TEXT, UI_C_WIN_BG);
        ui_draw_text(lcd, (uint16_t)(c->x + 8), (uint16_t)(pw.y + pw.h + 6),
                     "Type password, Enter to connect, Esc to cancel",
                     UI_C_TEXT_DIM, UI_C_WIN_BG);
        return;
    }

    arpile_wifi_ap_t ap;
    if (!arpile_wifi_get_ap(st->sel + st->scroll, &ap)) {
        st->view = WIFI_UI_VIEW_LIST;
        return;
    }
    char line[96];
    snprintf(line, sizeof(line), "Connect to %s", ap.ssid);
    ui_draw_text(lcd, (uint16_t)(c->x + 8), (uint16_t)(c->y + 8),
                 line, UI_C_TEXT, UI_C_WIN_BG);

    /* password field */
    ui_rect_t pw = { (uint16_t)(c->x + 8), (uint16_t)(c->y + CHAR_H + 18),
                     (uint16_t)(c->w - 16), (uint16_t)(CHAR_H + 8) };
    ui_draw_fill_rect(lcd, &pw, UI_C_WIN_BG);
    ui_draw_outline(lcd, &pw, UI_C_ACCENT);
    char masked[WIFI_UI_PW_MAX + 1];
    int mlen = st->pw_len;
    if (mlen > (int)sizeof(masked) - 1) mlen = (int)sizeof(masked) - 1;
    for (int i = 0; i < mlen; i++) masked[i] = '*';
    masked[mlen] = 0;
    ui_draw_text(lcd, (uint16_t)(pw.x + 4), (uint16_t)(pw.y + 4),
                 masked, UI_C_TEXT, UI_C_WIN_BG);

    /* hint line */
    char hint[WIFI_UI_MSG_LEN];
    snprintf(hint, sizeof(hint), "Type password, Enter to connect, Esc to cancel");
    ui_draw_text(lcd, (uint16_t)(c->x + 8), (uint16_t)(pw.y + pw.h + 6),
                 hint, UI_C_TEXT, UI_C_WIN_BG);
}

/* ---------------- event ---------------- */

static void wifi_event(arpile_app_ctx_t *ctx, const arpile_input_event_t *ev)
{
    wifi_state_t *st = ctx->user;
    if (!st) return;

    if (ev->type == ARPILE_IN_EVENT_KEY_DOWN) {
        uint8_t m = ev->key.modifier;
        bool ctrl = (m & (ARPILE_MOD_LCTRL | ARPILE_MOD_RCTRL)) ? true : false;

        if (st->view == WIFI_UI_VIEW_LIST) {
            switch (ev->key.keycode) {
            case ARPILE_KEY_UP:
                list_set_index(st, list_index(st) - 1);
                arpile_ui_win_redraw(ctx->win);
                return;
            case ARPILE_KEY_DOWN:
                list_set_index(st, list_index(st) + 1);
                arpile_ui_win_redraw(ctx->win);
                return;
            case ARPILE_KEY_PGUP:
                list_set_index(st, list_index(st) - 1);
                arpile_ui_win_redraw(ctx->win);
                return;
            case ARPILE_KEY_PGDN:
                list_set_index(st, list_index(st) + 1);
                arpile_ui_win_redraw(ctx->win);
                return;
            case ARPILE_KEY_ENTER:
                do_choose(st);
                arpile_ui_win_redraw(ctx->win);
                return;
            case ARPILE_KEY_ESCAPE:
                arpile_app_close(ctx);
                return;
            default: break;
            }
            if (ev->key.ascii && (ev->key.ascii == 's' || ev->key.ascii == 'S') && ctrl) {
                do_scan();
                arpile_ui_win_redraw(ctx->win);
            }
            /* Ctrl+I: manual connect by typing an SSID directly. */
            if (ctrl && ev->key.keycode == 0x0C) {   /* HID 'i' = 0x0C */
                st->view = WIFI_UI_VIEW_SSID;
                st->ssid_len = 0;
                st->ssid[0] = 0;
                st->pw_len = 0;
                arpile_ui_win_redraw(ctx->win);
            }
            return;
        }

        /* text-entry views (password for selected AP / manual SSID / manual password) */
        if (ev->key.keycode == ARPILE_KEY_ENTER) {
            if (st->view == WIFI_UI_VIEW_SSID) {
                if (st->ssid_len > 0) {
                    st->ssid[st->ssid_len] = 0;
                    st->view = WIFI_UI_VIEW_SSID_PASS;
                    st->pw_len = 0;
                }
            } else if (st->view == WIFI_UI_VIEW_SSID_PASS) {
                st->password[st->pw_len] = 0;
                arpile_wifi_connect(st->ssid, st->password);
                st->view = WIFI_UI_VIEW_LIST;
                st->pw_len = 0;
            } else {
                do_connect(st);
                st->view = WIFI_UI_VIEW_LIST;
                st->pw_len = 0;
            }
            arpile_ui_win_redraw(ctx->win);
            return;
        }
        if (ev->key.keycode == ARPILE_KEY_ESCAPE) {
            st->view = WIFI_UI_VIEW_LIST;
            st->pw_len = 0;
            arpile_ui_win_redraw(ctx->win);
            return;
        }
        if (ev->key.keycode == ARPILE_KEY_BACKSPACE || ev->key.ascii == 8) {
            if (st->view == WIFI_UI_VIEW_SSID) {
                if (st->ssid_len > 0) st->ssid_len--;
            } else if (st->pw_len > 0) {
                st->pw_len--;
            }
            arpile_ui_win_redraw(ctx->win);
            return;
        }
        if (ev->key.ascii >= 32 && ev->key.ascii < 127) {
            if (st->view == WIFI_UI_VIEW_SSID) {
                if (st->ssid_len < WIFI_UI_SSID_MAX - 1) {
                    st->ssid[st->ssid_len++] = ev->key.ascii;
                    st->ssid[st->ssid_len] = 0;
                }
            } else if (st->pw_len < WIFI_UI_PW_MAX) {
                st->password[st->pw_len++] = ev->key.ascii;
                st->password[st->pw_len] = 0;
            }
            arpile_ui_win_redraw(ctx->win);
        }
        return;
    }

    if (ev->type == ARPILE_IN_EVENT_MOUSE_BTN) {
        bool pressed = (ev->mouse.buttons & ARPILE_MOUSE_BTN_LEFT) ? true : false;
        if (!pressed) return;

        ui_rect_t c = win_client_rect(ctx->win);
        int mx = ev->mouse.x, my = ev->mouse.y;

        /* Scan button */
        ui_rect_t b = { (uint16_t)(c.x + 4), (uint16_t)(c.y + 2), 48, (uint16_t)(CHAR_H + 4) };
        if (ui_rect_contains(&b, mx, my)) {
            do_scan();
            arpile_ui_win_redraw(ctx->win);
            return;
        }

        if (st->view == WIFI_UI_VIEW_LIST) {
            int rows = (c.h - CHAR_H - 12) / WIFI_UI_ROW_H;
            if (rows > WIFI_UI_AP_ROWS) rows = WIFI_UI_AP_ROWS;
            int rel_y = my - (int)(c.y + CHAR_H + 14);
            if (rel_y >= 0) {
                int row = rel_y / WIFI_UI_ROW_H;
                if (row < rows) {
                    st->sel = row;
                    int idx = st->scroll + row;
                    if (idx < arpile_wifi_get_ap_count()) {
                        st->view = WIFI_UI_VIEW_PASS;
                        st->pw_len = 0;
                    }
                    arpile_ui_win_redraw(ctx->win);
                }
            }
        } else {
            /* password view: click outside -> back to list */
            ui_rect_t pw = { (uint16_t)(c.x + 8), (uint16_t)(c.y + CHAR_H + 18),
                             (uint16_t)(c.w - 16), (uint16_t)(CHAR_H + 8) };
            if (!ui_rect_contains(&pw, mx, my)) {
                st->view = WIFI_UI_VIEW_LIST;
                st->pw_len = 0;
                arpile_ui_win_redraw(ctx->win);
            }
        }
    }
}

static void wifi_render(arpile_app_ctx_t *ctx, ui_win_t *win)
{
    wifi_state_t *st = ctx->user;
    if (!st) return;
    ili9488_t *lcd = arpile_ui_get_lcd();
    ui_rect_t c = win_client_rect(win);
    ui_draw_fill_rect(lcd, &c, UI_C_WIN_BG);

    paint_header(lcd, &c);
    if (st->view == WIFI_UI_VIEW_LIST) {
        paint_list(lcd, &c, st);
    } else {
        paint_pass_view(lcd, &c, st);
    }
}

static void wifi_update(arpile_app_ctx_t *ctx)
{
    wifi_state_t *st = ctx->user;
    if (!st) return;
    arpile_wifi_poll();
    arpile_wifi_state_t st_ = arpile_wifi_get_state();
    static arpile_wifi_state_t s_last_state = (arpile_wifi_state_t)-1;
    static bool s_last_scan = false;
    static int s_last_count = -1;
    bool scan = arpile_wifi_scan_in_progress();
    int count = arpile_wifi_get_ap_count();

    /* Auto-rescan every 7s while open in case the first scan failed or no
     * networks were returned (esp_hosted/remote scans can be flaky at boot).
     * Long enough for a full channel sweep (~3s) plus a settle pause. */
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * 1000 / configTICK_RATE_HZ);
    if (!scan && s_scan_last_ms && (now_ms - s_scan_last_ms) > 7000) {
        do_scan();
        s_scan_last_ms = now_ms;
        snprintf(st->msg, sizeof(st->msg), "Scanning...");
    }

    if (st_ != s_last_state || scan != s_last_scan || count != s_last_count) {
        s_last_state = st_;
        s_last_scan = scan;
        s_last_count = count;
        arpile_ui_win_redraw(ctx->win);
    }
}

static void wifi_init(arpile_app_ctx_t *ctx)
{
    wifi_state_t *st = calloc(1, sizeof(wifi_state_t));
    ctx->user = st;
    g_self = st;
    st->view = WIFI_UI_VIEW_LIST;
    st->sel = 0;
    st->scroll = 0;
    st->msg[0] = 0;
    st->ssid_len = 0;
    st->ssid[0] = 0;
    do_scan();
}

static void wifi_destroy(arpile_app_ctx_t *ctx)
{
    if (g_self == ctx->user) {
        g_self = NULL;
    }
    if (ctx->user) {
        free(ctx->user);
        ctx->user = NULL;
    }
}

static const arpile_app_ops_t wifi_ops = {
    .init = wifi_init,
    .update = wifi_update,
    .event = wifi_event,
    .render = wifi_render,
    .destroy = wifi_destroy,
};

const arpile_app_t arpile_app_wifi = {
    .id = "wifi",
    .name = "WiFi",
    .icon = "wifi",
    .ops = &wifi_ops,
};