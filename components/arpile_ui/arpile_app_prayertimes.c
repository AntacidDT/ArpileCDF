/* PrayerTimes ACEXE - prayer times via the Aladhan API.
 *
 * F4 opens a location entry (city[, country] or "lat, lon"), F5 refreshes,
 * F1 shows help. The last location is persisted in NVS. Wall-clock time is
 * anchored from the API's currentTime endpoint (network is required for
 * prayer times anyway, so no SNTP dependency).
 */

#include "arpile_ui.h"
#include "arpile_app.h"
#include "wifi_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "nvs.h"

#define PT_MAX_LOC     48
#define PT_HTTP_MAX    16384
#define PT_TIMEOUT_MS  10000

#define PT_C_HEADER  UI_RGB(0x28, 0x48, 0x6F)
#define PT_C_BANNER  UI_RGB(0x1D, 0x7A, 0x4F)
#define PT_C_ROW_ALT UI_RGB(0x27, 0x42, 0x5F)
#define PT_C_NEXT    UI_RGB(0x2A, 0x99, 0xE8)
#define PT_C_ERR     UI_RGB(0xE8, 0x5A, 0x4A)

static const char *k_prayers[6] =
    { "Fajr", "Sunrise", "Dhuhr", "Asr", "Maghrib", "Isha" };

typedef struct {
    volatile uint32_t dirty;
    volatile int      workers;
    volatile bool     shutdown;

    char loc[PT_MAX_LOC];        /* persisted user-facing location string */
    bool have_loc;

    /* fetched data */
    char times[6][8];            /* "HH:MM" */
    char date_str[24];
    char hijri[16];
    char method[48];
    bool have_data;
    bool stale;

    /* local clock anchored at fetch time */
    int32_t day_secs0;
    int64_t anchor_us;
    int32_t last_seen_secs;
    bool    clock_ok;

    /* UI state */
    bool help;
    bool editing;
    char input[PT_MAX_LOC];
    int  input_len;
    char status[72];
    int  next_idx;               /* 0..5 or -1 */
    int  next_in;                /* seconds until next prayer */
} pt_t;

static portMUX_TYPE g_pt_mux = portMUX_INITIALIZER_UNLOCKED;

static void pt_touch(pt_t *st) { st->dirty++; }

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void pt_set_status(pt_t *st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st->status, sizeof(st->status), fmt, ap);
    va_end(ap);
    pt_touch(st);
}

static size_t pt_url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            if (n + 2 > cap) return 0;
            out[n++] = (char)c;
        } else {
            if (n + 4 > cap) return 0;
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = '\0';
    return n;
}

/* "HH:MM" -> seconds of day, or -1 */
static int32_t pt_parse_hhmm(const char *s)
{
    if (!s || strlen(s) < 4) return -1;
    int h = atoi(s);
    const char *colon = strchr(s, ':');
    if (!colon) return -1;
    int m = atoi(colon + 1);
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 3600 + m * 60;
}

/* Parse user input into a timings URL query suffix. Accepts
 * "lat, lon" or "City" or "City, Country". Returns false on bad coords. */
static bool pt_build_query(const char *loc, char *q, size_t cap)
{
    const char *comma = strchr(loc, ',');
    if (comma) {
        char a[24], b[24];
        size_t la = comma - loc;
        if (la < sizeof(a)) {
            memcpy(a, loc, la); a[la] = '\0';
            snprintf(b, sizeof(b), "%s", comma + 1);
            char *ea, *eb;
            float lat = strtof(a, &ea), lon = strtof(b, &eb);
            if (ea != a && eb != b &&
                lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180) {
                snprintf(q, cap, "?latitude=%.5f&longitude=%.5f&method=2",
                         (double)lat, (double)lon);
                return true;
            }
        }
    }

    char enc_a[PT_MAX_LOC * 3], enc_b[PT_MAX_LOC * 3];
    if ((comma = strrchr(loc, ',')) != NULL) {
        /* city, country */
        size_t lc = comma - loc;
        if (lc == 0 || lc >= PT_MAX_LOC) return false;
        char city[PT_MAX_LOC], country[PT_MAX_LOC];
        memcpy(city, loc, lc); city[lc] = '\0';
        snprintf(country, sizeof(country), "%s", comma + 1);
        if (!pt_url_encode(city, enc_a, sizeof(enc_a))) return false;
        if (!pt_url_encode(country, enc_b, sizeof(enc_b))) return false;
        snprintf(q, cap, "?city=%s&country=%s&method=2", enc_a, enc_b);
    } else {
        if (!loc[0]) return false;
        if (!pt_url_encode(loc, enc_a, sizeof(enc_a))) return false;
        snprintf(q, cap, "?city=%s&method=2", enc_a);
    }
    return true;
}

/* Blocking GET into a fixed buffer (NUL-terminated). */
static bool pt_http_get(const char *url, char *buf, size_t cap,
                        size_t *out_len)
{
    typedef struct {
        char   *buf;
        size_t  cap, len;
        bool    failed;
    } rx_t;

    rx_t rx = { .buf = buf, .cap = cap - 1, .len = 0 };

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = PT_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_data = NULL,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;

    /* simple inline event handling via perform loop */
    esp_err_t err = ESP_OK;
    *out_len = 0;

    /* esp_http_client has no accumulate helper; read in chunks */
    err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(cli);
        return false;
    }
    int status = esp_http_client_fetch_headers(cli);
    (void)status;
    if (esp_http_client_get_status_code(cli) != 200) {
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return false;
    }
    int r;
    while ((r = esp_http_client_read(cli, buf + rx.len,
                                     (int)(rx.cap - rx.len))) > 0) {
        rx.len += (size_t)r;
        if (rx.len >= rx.cap) break;
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (rx.len == 0) return false;
    buf[rx.len] = '\0';
    *out_len = rx.len;
    return true;
}

/* ------------------------------------------------------------------ */
/* fetch worker                                                        */
/* ------------------------------------------------------------------ */

static void pt_compute_next(pt_t *st)
{
    st->next_idx = -1;
    st->next_in = 0;
    if (!st->have_data || !st->clock_ok) return;

    int64_t elapsed = (esp_timer_get_time() - st->anchor_us) / 1000000LL;
    if (elapsed < 0) elapsed = 0;
    int32_t now = (int32_t)((st->day_secs0 + elapsed) % 86400);

    for (int i = 0; i < 6; i++) {
        int32_t t = pt_parse_hhmm(st->times[i]);
        if (t < 0) continue;
        if (t > now) {
            st->next_idx = i;
            st->next_in = t - now;
            return;
        }
    }
}

static void pt_fetch_task(void *arg)
{
    pt_t *st = (pt_t *)arg;
    char q[PT_MAX_LOC * 6 + 32];
    char url[PT_MAX_LOC * 6 + 96];
    char *body = malloc(PT_HTTP_MAX);

    if (!body) {
        pt_set_status(st, "Out of memory");
        goto out_fail_mark;
    }

    pt_set_status(st, "Fetching...");

    if (!arpile_wifi_is_connected()) {
        pt_set_status(st, "No WiFi connection");
        goto out_fail_mark;
    }
    if (!pt_build_query(st->loc, q, sizeof(q))) {
        pt_set_status(st, "Invalid location format");
        goto out_fail_mark;
    }

    snprintf(url, sizeof(url),
             "https://api.aladhan.com/v1/timings%s", q);

    size_t blen = 0;
    if (!pt_http_get(url, body, sizeof(body), &blen)) {
        pt_set_status(st, "Network error - check WiFi");
        goto out_fail_mark;
    }

    {
        cJSON *root = cJSON_ParseWithLength(body, blen);
        if (!root) { pt_set_status(st, "Bad response"); goto out_fail_mark; }

        cJSON *code = cJSON_GetObjectItem(root, "code");
        cJSON *data = cJSON_GetObjectItem(root, "data");
        if (!cJSON_IsNumber(code) || code->valueint != 200 || !data) {
            cJSON_Delete(root);
            pt_set_status(st, "Location not found");
            goto out_fail_mark;
        }

        cJSON *timings = cJSON_GetObjectItem(data, "timings");
        bool ok = cJSON_IsObject(timings);
        if (ok) {
            for (int i = 0; i < 6 && ok; i++) {
                cJSON *t = cJSON_GetObjectItem(timings, k_prayers[i]);
                if (!cJSON_IsString(t) ||
                    pt_parse_hhmm(t->valuestring) < 0) ok = false;
                else snprintf(st->times[i], sizeof(st->times[i]),
                              "%s", t->valuestring);
            }
        }
        if (!ok) {
            cJSON_Delete(root);
            pt_set_status(st, "Bad response");
            goto out_fail_mark;
        }

        cJSON *date = cJSON_GetObjectItem(data, "date");
        cJSON *readable = date ? cJSON_GetObjectItem(date, "readable") : NULL;
        cJSON *hijri = date ? cJSON_GetObjectItem(date, "hijri") : NULL;
        cJSON *hdate = hijri ? cJSON_GetObjectItem(hijri, "date") : NULL;

        cJSON *meta = cJSON_GetObjectItem(data, "meta");
        cJSON *mtz = meta ? cJSON_GetObjectItem(meta, "timezone") : NULL;
        cJSON *mmeth = meta ? cJSON_GetObjectItem(meta, "method") : NULL;
        cJSON *mname = mmeth ? cJSON_GetObjectItem(mmeth, "name") : NULL;

        char tz[40] = "";
        if (cJSON_IsString(mtz)) snprintf(tz, sizeof(tz), "%s", mtz->valuestring);
        snprintf(st->date_str, sizeof(st->date_str), "%s",
                 cJSON_IsString(readable) ? readable->valuestring : "");
        snprintf(st->hijri, sizeof(st->hijri), "%s",
                 cJSON_IsString(hdate) ? hdate->valuestring : "");
        snprintf(st->method, sizeof(st->method), "%s",
                 cJSON_IsString(mname) ? mname->valuestring : "");
        cJSON_Delete(root);

        /* anchor local clock: HH:MM right now at that timezone */
        char tbuf[128];
        size_t tl = 0;
        st->clock_ok = false;
        if (tz[0]) {
            snprintf(url, sizeof(url),
                     "https://api.aladhan.com/v1/currentTime?zone=");
            tl = strlen(url);
            if (pt_url_encode(tz, url + tl, sizeof(url) - tl)) {
                if (pt_http_get(url, tbuf, sizeof(tbuf), &tl)) {
                    /* {"code":200,"status":"OK","data":"17:59"} or plain */
                    const char *d = strstr(tbuf, "\"data\"");
                    if (d && (d = strchr(d + 6, '"')) != NULL) {
                        int32_t secs = pt_parse_hhmm(d + 1);
                        if (secs >= 0) {
                            st->day_secs0 = secs;
                            st->anchor_us = esp_timer_get_time();
                            st->clock_ok = true;
                        }
                    }
                }
            }
        }
    }

    st->have_data = true;
    st->stale = false;
    st->have_loc = true;
    pt_compute_next(st);

    if (!st->clock_ok)
        pt_set_status(st, "Times loaded (no clock)");
    else
        pt_set_status(st, "");

    /* persist location */
    {
        nvs_handle_t h;
        if (nvs_open("ptimes", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "loc", st->loc);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    goto out_touch;

out_fail_mark:
    if (st->have_data) st->stale = true;

out_touch:
    free(body);
    pt_touch(st);
    portENTER_CRITICAL(&g_pt_mux);
    st->workers--;
    bool last_out = (st->shutdown && st->workers == 0);
    portEXIT_CRITICAL(&g_pt_mux);
    if (last_out) free(st);
    vTaskDelete(NULL);
}

static bool pt_spawn_fetch(pt_t *st)
{
    portENTER_CRITICAL(&g_pt_mux);
    if (st->shutdown || st->workers > 0) {
        portEXIT_CRITICAL(&g_pt_mux);
        return false;
    }
    st->workers++;
    portEXIT_CRITICAL(&g_pt_mux);
    if (xTaskCreate(pt_fetch_task, "pt_fetch", 16384, st, 5, NULL) != pdPASS) {
        portENTER_CRITICAL(&g_pt_mux);
        st->workers--;
        portEXIT_CRITICAL(&g_pt_mux);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* rendering                                                           */
/* ------------------------------------------------------------------ */

static void pt_fmt_countdown(int secs, char *out, size_t cap)
{
    if (secs < 0) secs = 0;
    snprintf(out, cap, "%02d:%02d:%02d",
             secs / 3600, (secs / 60) % 60, secs % 60);
}

static void pt_render(arpile_app_ctx_t *ctx, ui_win_t *win)
{
    ili9488_t *lcd = arpile_ui_get_lcd();
    pt_t *st = ctx->user;
    if (!st) return;
    ui_rect_t c = win_client_rect(win);

    ui_draw_fill_rect(lcd, &c, UI_C_WIN_BG);
    const int w = c.w;
    int y = c.y;

    /* next-prayer banner (also carries date / hijri) */
    ui_rect_t br = { c.x + 8, y + 4, c.w - 16, 46 };
    ui_draw_fill_rect(lcd, &br, PT_C_BANNER);
    {
        char cd[16];
        char line[96];
        if (st->next_idx >= 0) {
            pt_fmt_countdown(st->next_in, cd, sizeof(cd));
            snprintf(line, sizeof(line), "Next: %s %s  (%s)",
                     k_prayers[st->next_idx], st->times[st->next_idx], cd);
        } else if (st->have_data && st->clock_ok) {
            snprintf(line, sizeof(line), "All prayers passed for today");
        } else {
            snprintf(line, sizeof(line), "Next prayer --:--");
        }
        ui_draw_text(lcd, br.x + 12, br.y + 7, line, UI_C_TEXT, PT_C_BANNER);

        char sub[80];
        snprintf(sub, sizeof(sub), "%.30s   %.20s",
                 st->date_str[0] ? st->date_str : "--",
                 st->hijri[0] ? st->hijri : "");
        ui_draw_text(lcd, br.x + 12, br.y + 27, sub,
                     UI_C_TEXT_DIM, PT_C_BANNER);
    }
    y += 4 + 46 + 4;

    /* rows */
    int rowh = (c.y + c.h - y - 22) / 6;
    if (rowh < 18) rowh = 18;
    for (int i = 0; i < 6; i++) {
        ui_rect_t rr = { c.x + 8, y, c.w - 16, rowh };
        bool is_next = (i == st->next_idx);
        ui_draw_fill_rect(lcd, &rr,
                          is_next ? PT_C_NEXT :
                          ((i & 1) ? PT_C_ROW_ALT : UI_C_WIN_BG));
        uint16_t fg = is_next ? UI_C_TEXT : UI_C_TEXT_LIGHT;
        ui_draw_text(lcd, rr.x + 12, y + (rowh - CHAR_H) / 2,
                     k_prayers[i], fg,
                     is_next ? PT_C_NEXT :
                     ((i & 1) ? PT_C_ROW_ALT : UI_C_WIN_BG));
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%s",
                 st->have_data ? st->times[i] : "--:--");
        int tw = ui_text_width(tbuf);
        ui_draw_text(lcd, rr.x + rr.w - tw - 14, y + (rowh - CHAR_H) / 2,
                     tbuf, fg,
                     is_next ? PT_C_NEXT :
                     ((i & 1) ? PT_C_ROW_ALT : UI_C_WIN_BG));
        y += rowh;
    }

    /* footer status / hints */
    ui_rect_t fr = { c.x, c.y + c.h - 20, c.w, 20 };
    ui_draw_fill_rect(lcd, &fr, PT_C_HEADER);
    if (st->status[0]) {
        ui_draw_text(lcd, c.x + 10, fr.y + 4, st->status,
                     st->stale ? PT_C_ERR : UI_C_TEXT_DIM, PT_C_HEADER);
        if (st->stale && !strstr(st->status, "stale")) {
            char sb[96];
            snprintf(sb, sizeof(sb), "%s (stale)", st->status);
            ui_draw_fill_rect(lcd, &fr, PT_C_HEADER);
            ui_draw_text(lcd, c.x + 10, fr.y + 4, sb, PT_C_ERR, PT_C_HEADER);
        }
    } else {
        ui_draw_text(lcd, c.x + 10, fr.y + 4,
                     "F1 Help  F4 Location  F5 Refresh",
                     UI_C_TEXT_DIM, PT_C_HEADER);
        if (st->method[0]) {
            char mb[64];
            snprintf(mb, sizeof(mb), "%.36s", st->method);
            int tw = ui_text_width(mb);
            ui_draw_text(lcd, c.x + w - tw - 10, fr.y + 4, mb,
                         UI_C_TEXT_DIM, PT_C_HEADER);
        }
    }

    /* location entry overlay */
    if (st->editing) {
        int bw = 400, bh = 110;
        ui_rect_t box = { c.x + (c.w - bw) / 2, c.y + 40, bw, bh };
        ui_draw_fill_rect(lcd, &box, UI_C_PANEL_HI);
        ui_draw_outline(lcd, &box, UI_C_ACCENT);
        ui_draw_text(lcd, box.x + 14, box.y + 10, "Set Location",
                     UI_C_TEXT, UI_C_PANEL_HI);
        ui_draw_text(lcd, box.x + 14, box.y + 34,
                     "City, Country  or  lat, lon",
                     UI_C_TEXT_DIM, UI_C_PANEL_HI);

        ui_rect_t field = { box.x + 14, box.y + 56, bw - 28, 20 };
        ui_draw_fill_rect(lcd, &field, UI_C_WIN_BG);
        char shown[PT_MAX_LOC + 2];
        snprintf(shown, sizeof(shown), "%s_", st->input);
        ui_draw_text(lcd, field.x + 6, field.y + 3, shown,
                     UI_C_TEXT, UI_C_WIN_BG);

        ui_draw_text(lcd, box.x + 14, box.y + bh - 18,
                     "Enter OK   Esc Cancel",
                     UI_C_TEXT_DIM, UI_C_PANEL_HI);
    }

    if (st->help) {
        int bw = 380, bh = 150;
        ui_rect_t box = { c.x + (c.w - bw) / 2, c.y + 30, bw, bh };
        ui_draw_fill_rect(lcd, &box, UI_C_PANEL_HI);
        ui_draw_outline(lcd, &box, UI_C_ACCENT);
        const char *lines[] = {
            "Prayer Times Help",
            "",
            "F4  Set location (saved automatically)",
            "F5  Refresh now",
            "Esc Close window",
            "",
            "Location: \"City, Country\" or \"lat, lon\"",
            "Data: aladhan.com - method ISNA",
        };
        for (int i = 0; i < (int)(sizeof(lines) / sizeof(lines[0])); i++)
            ui_draw_text(lcd, box.x + 14, box.y + 10 + i * 16, lines[i],
                         i == 0 ? UI_C_TEXT : UI_C_TEXT_DIM, UI_C_PANEL_HI);
    }
}

/* ------------------------------------------------------------------ */
/* events                                                              */
/* ------------------------------------------------------------------ */

static void pt_submit_location(arpile_app_ctx_t *ctx)
{
    pt_t *st = ctx->user;
    st->input[st->input_len] = '\0';

    /* trim spaces */
    char *s = st->input;
    while (*s == ' ') s++;
    char *e = s + strlen(s);
    while (e > s && e[-1] == ' ') e--;
    *e = '\0';

    if (!s[0]) {
        st->editing = false;
        pt_touch(st);
        return;
    }

    snprintf(st->loc, sizeof(st->loc), "%s", s);
    pt_spawn_fetch(st);
    st->editing = false;
    pt_touch(st);
}

static void pt_event(arpile_app_ctx_t *ctx, const arpile_input_event_t *ev)
{
    pt_t *st = ctx->user;
    if (!st) return;

    if (ev->type == ARPILE_IN_EVENT_KEY_DOWN) {
        uint16_t k = ev->key.keycode;

        if (k == ARPILE_KEY_F1) {
            st->help = !st->help;
            arpile_ui_win_redraw(ctx->win);
            return;
        }

        if (st->editing) {
            if (k == ARPILE_KEY_ESCAPE) {
                st->editing = false;
                arpile_ui_win_redraw(ctx->win);
            } else if (k == ARPILE_KEY_ENTER) {
                pt_submit_location(ctx);
                arpile_ui_win_redraw(ctx->win);
            } else if (k == ARPILE_KEY_BACKSPACE || ev->key.ascii == 8) {
                if (st->input_len > 0) st->input_len--;
                st->input[st->input_len] = '\0';
                arpile_ui_win_redraw(ctx->win);
            } else if (ev->key.ascii >= 32 && ev->key.ascii < 127) {
                if (st->input_len < PT_MAX_LOC - 1) {
                    st->input[st->input_len++] = ev->key.ascii;
                    st->input[st->input_len] = '\0';
                }
                arpile_ui_win_redraw(ctx->win);
            }
            return;
        }

        switch (k) {
        case ARPILE_KEY_F4:
            st->editing = true;
            st->input_len = 0;
            st->input[0] = '\0';
            arpile_ui_win_redraw(ctx->win);
            break;
        case ARPILE_KEY_F5:
            if (st->loc[0])
                pt_spawn_fetch(st);
            else
                st->editing = true, st->input_len = 0, st->input[0] = '\0';
            arpile_ui_win_redraw(ctx->win);
            break;
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void pt_update(arpile_app_ctx_t *ctx)
{
    pt_t *st = ctx->user;
    if (!st || !ctx->win) return;

    if (!st->have_data || !st->clock_ok) {
        if (st->dirty) {
            st->dirty = 0;
            arpile_ui_win_redraw(ctx->win);
        }
        return;
    }

    int64_t elapsed = (esp_timer_get_time() - st->anchor_us) / 1000000LL;
    if (elapsed < 0) elapsed = 0;
    int32_t now = (int32_t)((st->day_secs0 + elapsed) % 86400);

    /* midnight wrap -> refresh once for the new day */
    if (now < st->last_seen_secs - 3600 && st->workers == 0 &&
        !st->shutdown && st->loc[0]) {
        st->last_seen_secs = now;
        pt_spawn_fetch(st);
        pt_touch(st);
    } else {
        st->last_seen_secs = now;
    }

    int prev_next = st->next_idx;
    int prev_in = st->next_in;
    pt_compute_next(st);

    if (st->dirty || st->next_idx != prev_next ||
        (st->next_idx >= 0 && st->next_in != prev_in)) {
        st->dirty = 0;
        arpile_ui_win_redraw(ctx->win);
    }
}

static void pt_init(arpile_app_ctx_t *ctx)
{
    pt_t *st = calloc(1, sizeof(pt_t));
    if (!st) return;
    ctx->user = st;

    nvs_handle_t h;
    size_t sl = sizeof(st->loc);
    if (nvs_open("ptimes", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "loc", st->loc, &sl) == ESP_OK && st->loc[0])
            st->have_loc = true;
        nvs_close(h);
    }

    if (st->have_loc)
        pt_spawn_fetch(st);
    else {
        st->editing = true;
        pt_set_status(st, "Press F4 and enter your location");
    }
}

static void pt_destroy(arpile_app_ctx_t *ctx)
{
    pt_t *st = ctx->user;
    if (!st) return;

    bool leak = false;
    portENTER_CRITICAL(&g_pt_mux);
    st->shutdown = true;
    if (st->workers > 0) leak = true;   /* worker frees when it exits */
    portEXIT_CRITICAL(&g_pt_mux);

    if (!leak) {
        free(st);
        ctx->user = NULL;
    }
}

static const arpile_app_ops_t pt_ops = {
    .init    = pt_init,
    .update  = pt_update,
    .event   = pt_event,
    .render  = pt_render,
    .destroy = pt_destroy,
};

const arpile_app_t arpile_app_prayertimes = {
    .id   = "prayertimes",
    .name = "Prayer Times",
    .icon = "prayertimes",
    .ops  = &pt_ops,
};
