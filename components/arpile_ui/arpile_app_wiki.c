/* Wiki ACEXE - Wikipedia client for Arpile.
 *
 * Network-only, RAM-transient state, zero caching (no SD, no flash).
 * Images: Wikipedia thumbnails -> HTTP -> ESP32-P4 hardware JPEG decoder
 * -> RGB565 -> framebuffer. Hardware decode ONLY; anything else is reported
 * as unsupported rather than falling back to software decoding.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "driver/jpeg_decode.h"
#include "cJSON.h"
#include "arpile_app.h"
#include "arpile_ui.h"
#include "wifi_test.h"

#define WIKI_MAX_QUERY      96
#define WIKI_MAX_URL        512
#define WIKI_MAX_TITLE      128
#define WIKI_MAX_SNIPPET    160
#define WIKI_MAX_RESULTS    15
#define WIKI_SCROLL_LINE_H  (CHAR_H + 2)
#define WIKI_TITLE_BAR_H    20
#define WIKI_HINT_H         16

/* RAM budgets (board has no PSRAM enabled) */
#define WIKI_HTTP_BUF_INIT  (24 * 1024)
#define WIKI_HTTP_BUF_MAX   (320 * 1024)
#define WIKI_IMG_MAX        4            /* decoded images kept per article */
#define WIKI_IMG_URL_LEN    192
#define WIKI_IMG_BOX_W      168          /* images scaled into this box */
#define WIKI_IMG_BOX_H      140
#define WIKI_IMG_BOX_LINES  6            /* vertical space an image occupies */
#define WIKI_IMG_DL_MAX     (128 * 1024) /* refuse absurdly large downloads */
#define WIKI_LINKS_MAX      48

typedef enum {
    WIKI_VIEW_SEARCH = 0,
    WIKI_VIEW_RESULTS,
    WIKI_VIEW_ARTICLE,
    WIKI_VIEW_ERROR,
} wiki_view_t;

typedef enum {
    WIKI_IMG_EMPTY = 0,
    WIKI_IMG_PENDING,     /* seen in markup, waiting for loader */
    WIKI_IMG_LOADING,
    WIKI_IMG_READY,
    WIKI_IMG_FAILED,      /* unsupported format / too big / network error */
} wiki_img_state_t;

typedef struct {
    char url[WIKI_IMG_URL_LEN];
    uint16_t *pix;                 /* RGB565 or NULL */
    int w, h;
    wiki_img_state_t state;
} wiki_img_t;

typedef struct {
    char title[WIKI_MAX_TITLE];
    char snippet[WIKI_MAX_SNIPPET];
} wiki_result_t;

typedef struct {
    char title[WIKI_MAX_TITLE];
    int scroll;
} wiki_hist_t;

typedef struct {
    ui_rect_t rect;
    const char *title;             /* points into article HTML, valid until refetch */
} wiki_link_t;

typedef struct {
    wiki_view_t view;

    /* search screen */
    char query[WIKI_MAX_QUERY];
    int query_len;
    int query_cursor;

    /* results */
    wiki_result_t results[WIKI_MAX_RESULTS];
    int num_results;
    int sel_result;
    int scroll_results;

    /* article */
    char *html;
    char article_title[WIKI_MAX_TITLE];
    int article_scroll;
    int article_total_lines;
    int pending_scroll;            /* scroll to restore after back-nav fetch */

    /* link hit-test map rebuilt every render */
    wiki_link_t links[WIKI_LINKS_MAX];
    volatile int num_links;

    /* images */
    wiki_img_t imgs[WIKI_IMG_MAX];

    /* worker coordination */
    volatile uint32_t dirty_epoch; /* bumped whenever state changed off-screen */
    volatile int workers;
    volatile bool shutdown;

    /* history (RAM only) */
    wiki_hist_t hist[24];
    int hist_len;
    int pending_scroll_valid;

    char error_msg[128];
    bool help_open;

    /* scratch HTTP text buffer reused by search/article fetches */
    char *scratch;
    size_t scratch_cap;
} wiki_state_t;

static const char *WIKI_API_SEARCH =
    "https://en.wikipedia.org/w/api.php?action=query&list=search&srsearch=%s&format=json&srlimit=15";
static const char *WIKI_API_PARSE =
    "https://en.wikipedia.org/w/api.php?action=parse&page=%s&prop=text&formatversion=2&format=json";

/* Wikipedia-like palette (RGB565) */
#define WIKI_C_BG       0xFFFFu   /* white */
#define WIKI_C_TEXT     0x0000u   /* black */
#define WIKI_C_DIM      0x8C51u   /* gray */
#define WIKI_C_GRAY_BG  0xE71Cu   /* light gray */
#define WIKI_C_BORDER   0x2104u   /* dark gray */
#define WIKI_C_BTN      0x2C3Bu   /* blue */
#define WIKI_C_BTN_BRD  0x1A6Eu
#define WIKI_C_LINK     0x0F39u   /* link blue #3366CC */

static wiki_state_t *g_wiki = NULL;
static portMUX_TYPE g_wiki_mux = portMUX_INITIALIZER_UNLOCKED;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static void wiki_touch(wiki_state_t *st) { st->dirty_epoch++; }

static void wiki_free_all(wiki_state_t *st)
{
    for (int i = 0; i < WIKI_IMG_MAX; i++) {
        if (st->imgs[i].pix) free(st->imgs[i].pix);
    }
    if (st->html) free(st->html);
    if (st->scratch) free(st->scratch);
    free(st);
}

static void wiki_url_encode(const char *in, char *out, size_t out_sz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < out_sz; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else if (c == ' ') {
            out[o++] = '%'; out[o++] = '2'; out[o++] = '0';
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = '\0';
}

static const char *ci_strstr(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl) return hay;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return hay;
    }
    return NULL;
}

static void wiki_strip_tags(char *s)
{
    char *w = s;
    bool in_tag = false;
    for (char *r = s; *r; r++) {
        if (*r == '<') { in_tag = true; continue; }
        if (*r == '>') { in_tag = false; continue; }
        if (!in_tag) {
            if (!strncmp(r, "&amp;", 5)) { *w++ = '&'; r += 4; }
            else if (!strncmp(r, "&quot;", 6)) { *w++ = '"'; r += 5; }
            else if (!strncmp(r, "&#39;", 5) || !strncmp(r, "&apos;", 6)) { *w++ = '\''; r += 4; }
            else *w++ = *r;
        }
    }
    *w = '\0';
}

/* ------------------------------------------------------------------ */
/* HTTP                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t len, cap, max_cap;
    bool failed, binary, owned;
} http_rx_t;

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    http_rx_t *rx = (http_rx_t *)evt->user_data;
    if (!rx || rx->failed || evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (!evt->data || evt->data_len <= 0) return ESP_OK;

    size_t need = rx->len + (size_t)evt->data_len;
    if (!rx->binary) need += 1;
    if (need > rx->max_cap) { rx->failed = true; return ESP_OK; }

    if (need > rx->cap && rx->owned) {
        size_t nc = rx->cap ? rx->cap : WIKI_HTTP_BUF_INIT;
        while (nc < need) nc *= 2;
        char *nb = realloc(rx->buf, nc);
        if (!nb) { rx->failed = true; return ESP_OK; }
        rx->buf = nb;
        rx->cap = nc;
    }
    memcpy(rx->buf + rx->len, evt->data, evt->data_len);
    rx->len += evt->data_len;
    if (!rx->binary) rx->buf[rx->len] = '\0';
    return ESP_OK;
}

/* Blocking GET. On success returns buffer (caller frees unless prealloc'd)
 * and length. Binary mode never NUL-terminates. */
static bool http_get(const char *url, char **out_buf, size_t *out_len,
                     bool binary, size_t max_cap,
                     char *prealloc, size_t prealloc_sz)
{
    http_rx_t rx = { 0 };
    rx.binary = binary;
    rx.max_cap = max_cap;
    if (prealloc && prealloc_sz) {
        rx.buf = prealloc;
        rx.cap = prealloc_sz;
        rx.owned = false;
    } else {
        rx.buf = malloc(WIKI_HTTP_BUF_INIT > max_cap ? max_cap : WIKI_HTTP_BUF_INIT);
        if (!rx.buf) return false;
        rx.cap = WIKI_HTTP_BUF_INIT > max_cap ? max_cap : WIKI_HTTP_BUF_INIT;
        rx.owned = true;
    }
    rx.len = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_evt,
        .user_data = &rx,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        if (rx.owned) free(rx.buf);
        return false;
    }
    esp_err_t err = esp_http_client_perform(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || rx.failed || rx.len == 0) {
        if (rx.owned) free(rx.buf);
        return false;
    }
    *out_buf = rx.buf;
    *out_len = rx.len;
    return true;
}

/* ------------------------------------------------------------------ */
/* hardware JPEG decode (ESP32-P4)                                     */
/* ------------------------------------------------------------------ */

/* Decode JPEG to RGB565 via the hardware decoder, scaled into the display
 * box. Returns malloc'd pixels or NULL if unsupported. NO software fallback. */
static uint16_t *wiki_jpeg_decode_hw(const uint8_t *jpg, size_t jpg_len,
                                     int *out_w, int *out_h)
{
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(jpg, jpg_len, &info) != ESP_OK)
        return NULL;
    if (info.width == 0 || info.height == 0 ||
        info.width > 400 || info.height > 400)
        return NULL;                            /* thumbnails only */

    jpeg_decode_engine_cfg_t eng = { .intr_priority = 0, .timeout_ms = 3000 };
    jpeg_decoder_handle_t dec = NULL;
    if (jpeg_new_decoder_engine(&eng, &dec) != ESP_OK)
        return NULL;

    uint16_t *result = NULL;
    size_t raw_sz = (size_t)info.width * info.height * 2u;
    jpeg_decode_memory_alloc_cfg_t mem_cfg = { 0 };
    uint8_t *raw = jpeg_alloc_decoder_mem(raw_sz, &mem_cfg, NULL);

    if (raw) {
        jpeg_decode_cfg_t dcfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
            .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
        };
        uint32_t out_size = 0;
        if (jpeg_decoder_process(dec, &dcfg, jpg, jpg_len,
                                 raw, raw_sz, &out_size) == ESP_OK) {
            float s = 1.0f;
            if (info.width > WIKI_IMG_BOX_W) s = (float)WIKI_IMG_BOX_W / info.width;
            if ((float)info.height * s > WIKI_IMG_BOX_H)
                s = (float)WIKI_IMG_BOX_H / info.height;
            int dw = info.width * s; if (dw < 1) dw = 1;
            int dh = info.height * s; if (dh < 1) dh = 1;

            result = malloc((size_t)dw * dh * sizeof(uint16_t));
            if (result) {
                for (int y = 0; y < dh; y++) {
                    const uint16_t *src = (const uint16_t *)raw +
                        (size_t)((uint32_t)y * info.height / dh) * info.width;
                    uint16_t *dst = result + (size_t)y * dw;
                    for (int x = 0; x < dw; x++)
                        dst[x] = src[(uint32_t)x * info.width / dw];
                }
                *out_w = dw;
                *out_h = dh;
            }
        }
        free(raw);                              /* allocated via jpeg_alloc_.. */
    }

    jpeg_del_decoder_engine(dec);
    if (!result) { *out_w = 0; *out_h = 0; }
    return result;
}

/* Rewrite a Wikimedia thumb URL to ~168px wide to keep downloads small:
 * .../thumb/X/XX/File.jpg/220px-File.jpg -> .../168px-File.jpg */
static void wiki_thumb_url(char *url, size_t url_sz)
{
    if (!strstr(url, "/thumb/")) return;
    for (char *p = url; (p = strstr(p, "px-")) != NULL; p += 3) {
        char *d = p;
        int digits = 0;
        while (d > url && isdigit((unsigned char)d[-1])) { d--; digits++; }
        if (digits >= 2 && digits <= 4) {
            char rest[WIKI_IMG_URL_LEN];
            strncpy(rest, d, sizeof(rest) - 1);
            rest[sizeof(rest) - 1] = '\0';
            snprintf(d, url_sz - (size_t)(d - url), "168%s", rest);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* image slots                                                         */
/* ------------------------------------------------------------------ */

static void wiki_imgs_reset(wiki_state_t *st)
{
    for (int i = 0; i < WIKI_IMG_MAX; i++) {
        if (st->imgs[i].pix) free(st->imgs[i].pix);
        memset(&st->imgs[i], 0, sizeof(st->imgs[i]));
    }
}

static int wiki_img_slot(wiki_state_t *st, const char *url)
{
    for (int i = 0; i < WIKI_IMG_MAX; i++)
        if (st->imgs[i].state != WIKI_IMG_EMPTY &&
            strcmp(st->imgs[i].url, url) == 0)
            return i;

    int pick = -1;
    for (int i = 0; i < WIKI_IMG_MAX && pick < 0; i++)
        if (st->imgs[i].state == WIKI_IMG_EMPTY) pick = i;
    for (int i = 0; i < WIKI_IMG_MAX && pick < 0; i++)
        if (st->imgs[i].state == WIKI_IMG_FAILED) pick = i;
    if (pick < 0) pick = 0;

    if (st->imgs[pick].pix) free(st->imgs[pick].pix);
    memset(&st->imgs[pick], 0, sizeof(st->imgs[pick]));
    strncpy(st->imgs[pick].url, url, WIKI_IMG_URL_LEN - 1);
    st->imgs[pick].state = WIKI_IMG_PENDING;
    return pick;
}

static void wiki_image_loader(void *arg)
{
    wiki_state_t *st = (wiki_state_t *)arg;

    for (;;) {
        if (st->shutdown) break;

        int slot = -1;
        for (int i = 0; i < WIKI_IMG_MAX; i++)
            if (st->imgs[i].state == WIKI_IMG_PENDING) { slot = i; break; }
        if (slot < 0) break;

        char url[WIKI_IMG_URL_LEN];
        strncpy(url, st->imgs[slot].url, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
        st->imgs[slot].state = WIKI_IMG_LOADING;

        char *buf = NULL;
        size_t len = 0;
        int iw = 0, ih = 0;
        uint16_t *pix = NULL;
        if (http_get(url, &buf, &len, true, WIKI_IMG_DL_MAX, NULL, 0)) {
            pix = wiki_jpeg_decode_hw((const uint8_t *)buf, len, &iw, &ih);
            free(buf);
        }

        if (strcmp(st->imgs[slot].url, url) != 0) {
            if (pix) free(pix);               /* slot was reused meanwhile */
        } else {
            st->imgs[slot].pix = pix;
            st->imgs[slot].w = iw;
            st->imgs[slot].h = ih;
            st->imgs[slot].state = pix ? WIKI_IMG_READY : WIKI_IMG_FAILED;
            wiki_touch(st);                   /* repaint from UI task */
        }
    }

    portENTER_CRITICAL(&g_wiki_mux);
    st->workers--;
    bool last_out = (st->shutdown && st->workers == 0);
    portEXIT_CRITICAL(&g_wiki_mux);
    if (last_out) wiki_free_all(st);
    vTaskDelete(NULL);
}

static void wiki_kick_loader(wiki_state_t *st)
{
    bool spawn = false;
    portENTER_CRITICAL(&g_wiki_mux);
    if (!st->shutdown && st->workers == 0) {
        st->workers++;
        spawn = true;
    }
    portEXIT_CRITICAL(&g_wiki_mux);
    if (spawn && xTaskCreate(wiki_image_loader, "wiki_img", 6144, st, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&g_wiki_mux);
        st->workers--;
        portEXIT_CRITICAL(&g_wiki_mux);
    }
}

/* ------------------------------------------------------------------ */
/* navigation                                                          */
/* ------------------------------------------------------------------ */

static void wiki_set_error(wiki_state_t *st, const char *msg)
{
    snprintf(st->error_msg, sizeof(st->error_msg), "%s", msg);
    st->view = WIKI_VIEW_ERROR;
    wiki_touch(st);
}

static void wiki_push_hist(wiki_state_t *st, const char *title, int scroll)
{
    if (!title[0]) return;
    if (st->hist_len < (int)(sizeof(st->hist) / sizeof(st->hist[0]))) {
        strncpy(st->hist[st->hist_len].title, title, WIKI_MAX_TITLE - 1);
        st->hist[st->hist_len].title[WIKI_MAX_TITLE - 1] = '\0';
        st->hist[st->hist_len].scroll = scroll;
        st->hist_len++;
    }
}

/* ------------------------------------------------------------------ */
/* worker tasks                                                        */
/* ------------------------------------------------------------------ */

static void wiki_search_task(void *arg)
{
    wiki_state_t *st = (wiki_state_t *)arg;
    char q_enc[WIKI_MAX_QUERY * 3];
    char url[WIKI_MAX_URL];

    wiki_url_encode(st->query, q_enc, sizeof(q_enc));
    snprintf(url, sizeof(url), WIKI_API_SEARCH, q_enc);

    char *body = NULL;
    size_t blen = 0;
    if (http_get(url, &body, &blen, false, WIKI_HTTP_BUF_MAX, NULL, 0)) {
        st->num_results = 0;
        cJSON *root = cJSON_Parse(body);
        free(body);
        if (root) {
            cJSON *q = cJSON_GetObjectItem(root, "query");
            cJSON *search = q ? cJSON_GetObjectItem(q, "search") : NULL;
            cJSON *it = NULL;
            cJSON_ArrayForEach(it, search) {
                if (st->num_results >= WIKI_MAX_RESULTS) break;
                cJSON *t = cJSON_GetObjectItem(it, "title");
                cJSON *s = cJSON_GetObjectItem(it, "snippet");
                if (!cJSON_IsString(t)) continue;
                wiki_result_t *r = &st->results[st->num_results];
                strncpy(r->title, t->valuestring, WIKI_MAX_TITLE - 1);
                r->title[WIKI_MAX_TITLE - 1] = '\0';
                r->snippet[0] = '\0';
                if (cJSON_IsString(s)) {
                    strncpy(r->snippet, s->valuestring, WIKI_MAX_SNIPPET - 1);
                    r->snippet[WIKI_MAX_SNIPPET - 1] = '\0';
                    wiki_strip_tags(r->snippet);
                }
                st->num_results++;
            }
            cJSON_Delete(root);
        }
        if (st->num_results > 0) {
            st->sel_result = 0;
            st->scroll_results = 0;
            st->view = WIKI_VIEW_RESULTS;
        } else {
            wiki_set_error(st, "No results found");
        }
    } else {
        wiki_set_error(st, "Search failed - check WiFi");
    }
    wiki_touch(st);

    portENTER_CRITICAL(&g_wiki_mux);
    st->workers--;
    bool last_out = (st->shutdown && st->workers == 0);
    portEXIT_CRITICAL(&g_wiki_mux);
    if (last_out) wiki_free_all(st);
    vTaskDelete(NULL);
}

static void wiki_article_task(void *arg)
{
    char *title = (char *)arg;
    char url[WIKI_MAX_URL];
    char t_enc[WIKI_MAX_TITLE * 3];
    char fetched[WIKI_MAX_TITLE] = "";
    char *newhtml = NULL;

    wiki_url_encode(title, t_enc, sizeof(t_enc));
    snprintf(url, sizeof(url), WIKI_API_PARSE, t_enc);

    char *body = NULL;
    size_t blen = 0;
    if (http_get(url, &body, &blen, false, WIKI_HTTP_BUF_MAX, NULL, 0)) {
        cJSON *root = cJSON_Parse(body);
        free(body);
        if (root) {
            cJSON *parse = cJSON_GetObjectItem(root, "parse");
            cJSON *jt = parse ? cJSON_GetObjectItem(parse, "title") : NULL;
            cJSON *text = parse ? cJSON_GetObjectItem(parse, "text") : NULL;
            if (cJSON_IsString(text)) {
                size_t hl = strlen(text->valuestring);
                if (hl > WIKI_HTTP_BUF_MAX) hl = WIKI_HTTP_BUF_MAX;
                newhtml = malloc(hl + 1);
                if (newhtml) {
                    memcpy(newhtml, text->valuestring, hl);
                    newhtml[hl] = '\0';
                    if (cJSON_IsString(jt))
                        strncpy(fetched, jt->valuestring, sizeof(fetched) - 1);
                }
            }
            cJSON_Delete(root);
        }
    }

    wiki_state_t *st = g_wiki;
    if (!st || st->shutdown || !newhtml) {
        if (newhtml) free(newhtml);
        free(title);
        if (st && !st->shutdown && !newhtml) {
            wiki_set_error(st, "Failed to load article");
            wiki_touch(st);
        }
        portENTER_CRITICAL(&g_wiki_mux);
        st = g_wiki;                             /* re-read: may be gone */
        if (st) {
            st->workers--;
            if (st->shutdown && st->workers == 0) {
                portEXIT_CRITICAL(&g_wiki_mux);
                wiki_free_all(st);
                vTaskDelete(NULL);
                return;
            }
        }
        portEXIT_CRITICAL(&g_wiki_mux);
        vTaskDelete(NULL);
        return;
    }

    /* install new article */
    if (st->html) free(st->html);
    st->html = newhtml;
    strncpy(st->article_title, fetched[0] ? fetched : title, WIKI_MAX_TITLE - 1);
    st->article_title[WIKI_MAX_TITLE - 1] = '\0';

    wiki_imgs_reset(st);
    st->num_links = 0;
    st->article_total_lines = 1;
    st->article_scroll = st->pending_scroll_valid ? st->pending_scroll : 0;
    st->pending_scroll_valid = 0;
    st->view = WIKI_VIEW_ARTICLE;
    wiki_touch(st);

    free(title);
    portENTER_CRITICAL(&g_wiki_mux);
    st = g_wiki;
    if (st) {
        st->workers--;
        if (st->shutdown && st->workers == 0) {
            portEXIT_CRITICAL(&g_wiki_mux);
            wiki_free_all(st);
            vTaskDelete(NULL);
            return;
        }
    }
    portEXIT_CRITICAL(&g_wiki_mux);
    vTaskDelete(NULL);
}

static void wiki_fetch_article(wiki_state_t *st, const char *title)
{
    if (!title || !title[0]) return;
    char *t = malloc(WIKI_MAX_TITLE);
    if (!t) return;
    strncpy(t, title, WIKI_MAX_TITLE - 1);
    t[WIKI_MAX_TITLE - 1] = '\0';

    bool spawned = false;
    portENTER_CRITICAL(&g_wiki_mux);
    if (!st->shutdown) {
        st->workers++;
        spawned = true;
    }
    portEXIT_CRITICAL(&g_wiki_mux);
    if (!spawned) { free(t); return; }

    if (xTaskCreate(wiki_article_task, "wiki_art", 16384, t, 5, NULL) != pdPASS) {
        portENTER_CRITICAL(&g_wiki_mux);
        st->workers--;
        bool die = (st->shutdown && st->workers == 0);
        portEXIT_CRITICAL(&g_wiki_mux);
        if (die) wiki_free_all(st);
        free(t);
    }
}

static void wiki_do_search(wiki_state_t *st)
{
    if (st->query_len == 0) return;
    st->query[st->query_len] = '\0';

    bool spawned = false;
    portENTER_CRITICAL(&g_wiki_mux);
    if (!st->shutdown) {
        st->workers++;
        spawned = true;
    }
    portEXIT_CRITICAL(&g_wiki_mux);
    if (!spawned) return;
    if (xTaskCreate(wiki_search_task, "wiki_srch", 12288, st, 5, NULL) != pdPASS) {
        portENTER_CRITICAL(&g_wiki_mux);
        st->workers--;
        portEXIT_CRITICAL(&g_wiki_mux);
    }
}

static void wiki_open_result(wiki_state_t *st, int idx)
{
    if (idx < 0 || idx >= st->num_results) return;
    st->pending_scroll_valid = 0;
    wiki_fetch_article(st, st->results[idx].title);
}

static void wiki_go_back(wiki_state_t *st)
{
    switch (st->view) {
    case WIKI_VIEW_ARTICLE:
        if (st->hist_len > 0) {
            st->hist_len--;
            wiki_hist_t *h = &st->hist[st->hist_len];
            st->pending_scroll = h->scroll;
            st->pending_scroll_valid = 1;
            st->article_scroll = 0;
            wiki_fetch_article(st, h->title);
        } else {
            st->view = st->num_results ? WIKI_VIEW_RESULTS : WIKI_VIEW_SEARCH;
            wiki_touch(st);
        }
        break;
    case WIKI_VIEW_RESULTS:
        st->view = WIKI_VIEW_SEARCH;
        wiki_touch(st);
        break;
    case WIKI_VIEW_ERROR:
        st->view = st->num_results ? WIKI_VIEW_RESULTS : WIKI_VIEW_SEARCH;
        wiki_touch(st);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* HTML renderer                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    ili9488_t *lcd;
    ui_rect_t clip;                /* paintable article area */
    int line_h;
    int x0, right;
    int cur_x;
    int line;                      /* absolute line index */
    int scroll;
    bool space_pending;
    bool bullet_pending;
    char bullet[8];
    bool in_link;
    char link_title[WIKI_MAX_TITLE];
    bool in_heading;
    wiki_link_t *links;
    int num_links;
} lay_t;

static int lay_screen_y(lay_t *L)
{
    return L->clip.y + (L->line - L->scroll) * L->line_h;
}

static bool lay_visible(lay_t *L)
{
    int y = lay_screen_y(L);
    return y >= L->clip.y - L->line_h && y <= L->clip.y + L->clip.h;
}

static void lay_break(lay_t *L)
{
    L->line++;
    L->cur_x = L->x0;
    L->space_pending = false;
}

static void lay_gap(lay_t *L)                    /* paragraph-style gap */
{
    if (L->cur_x > L->x0) lay_break(L);
    lay_break(L);
}

static void lay_add_link_rect(lay_t *L, int x, int w)
{
    if (L->num_links >= WIKI_LINKS_MAX) return;
    int y = lay_screen_y(L);
    for (int i = L->num_links - 1; i >= 0; i--) {
        wiki_link_t *l = &L->links[i];
        if (l->rect.y != y) break;
        if (l->rect.x + l->rect.w == x && strcmp(l->title, L->link_title) == 0) {
            l->rect.w += w;
            return;
        }
    }
    wiki_link_t *l = &L->links[L->num_links++];
    l->title = L->link_title;
    l->rect.x = (uint16_t)x;
    l->rect.y = (uint16_t)y;
    l->rect.w = (uint16_t)w;
    l->rect.h = (uint16_t)L->line_h;
}

static void lay_word(lay_t *L, const char *word)
{
    int wpx = ui_text_width(word);
    int sp = L->space_pending ? CHAR_W : 0;

    if (L->cur_x - L->x0 + sp + wpx > L->right - L->x0 && L->cur_x > L->x0)
        lay_break(L);

    int y = lay_screen_y(L);
    bool vis = lay_visible(L);
    int dx = L->cur_x;

    if (L->bullet_pending && L->cur_x == L->x0) {
        if (vis)
            ui_draw_text(L->lcd, (uint16_t)L->x0, (uint16_t)y, L->bullet,
                         WIKI_C_DIM, WIKI_C_BG);
        dx = L->x0 + ui_text_width(L->bullet);
        L->bullet_pending = false;
    }

    if (vis) {
        ui_draw_text(L->lcd, (uint16_t)dx, (uint16_t)y, word,
                     L->in_link ? WIKI_C_LINK : WIKI_C_TEXT, WIKI_C_BG);
        if (L->in_link) {
            for (int ux = dx; ux < dx + wpx; ux += 2)
                ui_draw_vline(L->lcd, (uint16_t)ux, (uint16_t)(y + L->line_h - 2),
                              1, WIKI_C_LINK);
            lay_add_link_rect(L, dx, wpx);
        }
        if (L->in_heading)
            ui_draw_hline(L->lcd, (uint16_t)dx, (uint16_t)(y + L->line_h - 1),
                          (uint16_t)wpx, WIKI_C_BORDER);
    }

    L->cur_x = dx + wpx;
    L->space_pending = true;
}

/* Render one <img>: placeholder or decoded bitmap; reserves vertical space. */
static void lay_image(lay_t *L, wiki_state_t *st, const char *src)
{
    if (L->cur_x > L->x0) lay_break(L);

    char thumb[WIKI_IMG_URL_LEN];
    strncpy(thumb, src, sizeof(thumb) - 1);
    thumb[sizeof(thumb) - 1] = '\0';
    wiki_thumb_url(thumb, sizeof(thumb));

    int slot = wiki_img_slot(st, thumb);
    int y = lay_screen_y(L);
    const int box_h = WIKI_IMG_BOX_LINES * L->line_h;
    bool fully_vis = y >= L->clip.y && y + box_h <= L->clip.y + L->clip.h;

    if (fully_vis && slot >= 0 &&
        st->imgs[slot].state == WIKI_IMG_READY && st->imgs[slot].pix) {
        wiki_img_t *im = &st->imgs[slot];
        int dx = L->x0 + 8, dy = y + 2;
        int w = im->w, h = im->h;
        if (dx + w > L->right) w = L->right - dx;
        if (w > 0 && h > 0 && dy + h <= L->clip.y + L->clip.h)
            ui_fb_blit(im->pix, (uint16_t)dx, (uint16_t)dy,
                       (uint16_t)w, (uint16_t)h,
                       0, 0, (uint16_t)w, (uint16_t)h, (uint16_t)im->w);
    } else if (fully_vis) {
        ui_rect_t box = { (uint16_t)(L->x0 + 4), (uint16_t)(y + 2),
                          (uint16_t)(WIKI_IMG_BOX_W + 12), (uint16_t)(box_h - 4) };
        if (box.x + box.w > L->right) box.w = (uint16_t)(L->right - box.x);
        ui_draw_fill_rect(L->lcd, &box, WIKI_C_GRAY_BG);
        ui_draw_outline(L->lcd, &box, WIKI_C_DIM);
        const char *lbl =
            (slot >= 0 && st->imgs[slot].state == WIKI_IMG_FAILED)
                ? "[image unsupported]"
                : "[image]";
        ui_draw_text(L->lcd, (uint16_t)(box.x + 8),
                     (uint16_t)(box.y + box.h / 2 - CHAR_H / 2),
                     lbl, WIKI_C_DIM, WIKI_C_GRAY_BG);
    }

    L->line += WIKI_IMG_BOX_LINES;
    L->cur_x = L->x0;
    L->space_pending = false;
}

static void lay_parse_href(lay_t *L, const char *tag)
{
    L->in_link = false;
    const char *href = strstr(tag, "href=\"");
    char q = '"';
    if (!href) { href = strstr(tag, "href='"); q = '\''; }
    if (!href) return;
    href += 6;

    if (strncmp(href, "/wiki/", 6) != 0) return;
    href += 6;

    size_t n = 0;
    while (href[n] && href[n] != q && href[n] != '#' && href[n] != '?' &&
           n < sizeof(L->link_title) - 1)
        n++;
    if (memchr(href, ':', n)) return;            /* Special:, File:, etc */

    memcpy(L->link_title, href, n);
    for (size_t i = 0; i < n; i++)
        if (L->link_title[i] == '_') L->link_title[i] = ' ';
    L->link_title[n] = '\0';
    L->in_link = true;
}

static void lay_img_src(const char *tag, char *out, size_t out_sz)
{
    out[0] = '\0';
    const char *src = strstr(tag, "src=\"");
    char q = '"';
    if (!src) { src = strstr(tag, "src='"); q = '\''; }
    if (!src) return;
    src += 5;
    size_t n = 0;
    while (src[n] && src[n] != q && n < out_sz - 7) n++;

    if (n == 0) return;
    if (src[0] == '/' && src[1] == '/') {
        memcpy(out, "https:", 6);
        memcpy(out + 6, src, n);
        out[n + 6] = '\0';
    } else {
        memcpy(out, src, n);
        out[n] = '\0';
    }
}

static char lay_entity_char(const char *name)
{
    if (!strcmp(name, "amp")) return '&';
    if (!strcmp(name, "lt")) return '<';
    if (!strcmp(name, "gt")) return '>';
    if (!strcmp(name, "quot")) return '"';
    if (!strcmp(name, "apos")) return '\'';
    if (!strcmp(name, "nbsp")) return ' ';
    if (!strcmp(name, "ndash") || !strcmp(name, "mdash")) return '-';
    return 0;
}

/* Streaming layout+paint over st->html. Rebuilds the link map. Kicks the
 * image loader if any new thumbnails were registered. */
static void wiki_render_html(ili9488_t *lcd, const ui_rect_t *area,
                             wiki_state_t *st, int scroll)
{
    lay_t L = { 0 };
    L.lcd = lcd;
    L.clip = *area;
    L.line_h = WIKI_SCROLL_LINE_H;
    L.x0 = area->x + 4;
    L.right = area->x + area->w - 4;
    L.cur_x = L.x0;
    L.scroll = scroll;
    L.links = st->links;
    L.num_links = 0;
    strcpy(L.bullet, "* ");

    st->num_links = 0;

    char word[80];
    int wl = 0;
    bool kick = false;
    const char *p = st->html;

    while (*p) {
        if (*p == '<') {
            if (wl) { word[wl] = '\0'; lay_word(&L, word); wl = 0; }

            if (!strncmp(p, "<!--", 4)) {
                const char *e = ci_strstr(p, "-->");
                if (!e) break;
                p = e + 3;
                continue;
            }
            const char *gt = strchr(p, '>');
            if (!gt) break;

            char tag[192];
            size_t tl = (size_t)(gt - p) - 1;
            if (tl >= sizeof(tag)) tl = sizeof(tag) - 1;
            char *tg = tag;
            memcpy(tag, p + 1, tl);
            tag[tl] = '\0';

            /* lowercase copy for name tests (tag body kept original for attrs) */
            /* extract lowercase tag name (skips leading '/' on close tags) */
            char low[24];
            size_t li = 0;
            size_t ti = 0;
            bool closing = false;
            if (tg[0] == '/') { closing = true; ti = 1; }
            for (; tg[ti] && li < sizeof(low) - 1; li++, ti++) {
                char ch = tg[ti];
                if (ch == ' ' || ch == '/' || ch == '>') break;
                low[li] = (char)tolower((unsigned char)ch);
            }
            low[li] = '\0';

            p = gt + 1;

            if (closing) {
                if (!strcmp(low, "p") || !strcmp(low, "div") ||
                    !strcmp(low, "table") || !strcmp(low, "ul") ||
                    !strcmp(low, "ol") || !strcmp(low, "blockquote") ||
                    (low[0] == 'h' && low[1] >= '1' && low[1] <= '6' && !low[2])) {
                    lay_gap(&L);
                    L.in_heading = false;
                } else if (!strcmp(low, "li")) {
                    lay_gap(&L);
                    L.bullet_pending = false;
                } else if (!strcmp(low, "tr")) {
                    lay_gap(&L);
                } else if (!strcmp(low, "td") || !strcmp(low, "th")) {
                    lay_break(&L);
                } else if (!strcmp(low, "a")) {
                    L.in_link = false;
                    L.link_title[0] = '\0';
                } else if (low[0] == 'h' && low[1] >= '1' && low[1] <= '6') {
                    L.in_heading = false;
                }
                continue;
            }

            /* skip scripts/styles entirely */
            if (!strcmp(low, "script") || !strcmp(low, "style")) {
                char close[32];
                snprintf(close, sizeof(close), "</%s>", low);
                const char *e = ci_strstr(p, close);
                p = e ? e + strlen(close) : p + strlen(p);
                continue;
            }

            if (!strcmp(low, "h1") || !strcmp(low, "h2") ||
                !strcmp(low, "h3") || !strcmp(low, "h4")) {
                lay_gap(&L);
                L.in_heading = true;
            } else if (!strcmp(low, "br") || !strcmp(low, "br/")) {
                lay_break(&L);
            } else if (!strcmp(low, "p") || !strcmp(low, "div") ||
                       !strcmp(low, "blockquote")) {
                lay_gap(&L);
            } else if (!strcmp(low, "ul") || !strcmp(low, "ol")) {
                lay_gap(&L);
            } else if (!strcmp(low, "li")) {
                lay_gap(&L);
                strcpy(L.bullet, "* ");
                L.bullet_pending = true;
            } else if (!strcmp(low, "table")) {
                lay_gap(&L);
            } else if (!strcmp(low, "tr")) {
                lay_gap(&L);
            } else if (!strcmp(low, "td") || !strcmp(low, "th")) {
                /* cell content flows inline; break before next cell handled at </td> */
            } else if (!strcmp(low, "a")) {
                lay_parse_href(&L, tag);
            } else if (!strcmp(low, "img")) {
                char src[WIKI_IMG_URL_LEN];
                lay_img_src(tag, src, sizeof(src));
                if (src[0] && strstr(src, "upload.wikimedia.org")) {
                    lay_image(&L, st, src);
                    kick = true;
                }
            }
            /* everything else (span, sup, references markup...) renders inline */
            continue;
        }

        /* raw text */
        if (*p == '\n' || *p == '\r' || *p == '\t') { p++; continue; }
        if (*p == ' ') {
            if (wl) { word[wl] = '\0'; lay_word(&L, word); wl = 0; }
            L.space_pending = true;
            p++;
            continue;
        }
        if (*p == '&') {
            const char *semi = strchr(p, ';');
            if (semi && semi - p <= 10) {
                char ent[12];
                size_t el = (size_t)(semi - p) - 1;
                memcpy(ent, p + 1, el);
                ent[el] = '\0';
                char c = ent[0] == '#' ? ((atoi(ent + 1) >= 32 && atoi(ent + 1) < 127)
                                          ? (char)atoi(ent + 1) : 0)
                                       : lay_entity_char(ent);
                if (c == ' ') {
                    if (wl) { word[wl] = '\0'; lay_word(&L, word); wl = 0; }
                    L.space_pending = true;
                } else if (c && wl < (int)sizeof(word) - 1) {
                    word[wl++] = c;
                }
                p = semi + 1;
                continue;
            }
        }
        if (wl < (int)sizeof(word) - 1) {
            word[wl++] = *p++;
        } else {
            word[wl] = '\0';
            lay_word(&L, word);
            wl = 0;
        }
    }
    if (wl) { word[wl] = '\0'; lay_word(&L, word); }
    lay_gap(&L);

    st->article_total_lines = L.line + 1;
    st->num_links = L.num_links;
    if (kick) wiki_kick_loader(st);
}

/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */

static ui_rect_t wiki_search_field(const ui_rect_t *c)
{
    return (ui_rect_t){ (uint16_t)(c->x + 30), (uint16_t)(c->y + 70),
                        (uint16_t)(c->w - 60), 34 };
}

static ui_rect_t wiki_search_button(const ui_rect_t *c)
{
    return (ui_rect_t){ (uint16_t)(c->x + (c->w - 110) / 2), (uint16_t)(c->y + 130),
                        110, 34 };
}

static ui_rect_t wiki_article_area(const ui_rect_t *c)
{
    return (ui_rect_t){ (uint16_t)(c->x + 2), (uint16_t)(c->y + WIKI_TITLE_BAR_H),
                        (uint16_t)(c->w - 4),
                        (uint16_t)(c->h - WIKI_TITLE_BAR_H - WIKI_HINT_H) };
}

static int wiki_rows_visible(const ui_rect_t *c)
{
    int rows = (c->h - 40) / 38;
    if (rows < 1) rows = 1;
    if (rows > WIKI_MAX_RESULTS) rows = WIKI_MAX_RESULTS;
    return rows;
}

static void wiki_draw_search(ili9488_t *lcd, const ui_rect_t *c, wiki_state_t *st)
{
    ui_draw_fill_rect(lcd, c, WIKI_C_BG);

    const char *logo = "W I K I P E D I A";
    uint16_t tw = ui_text_width(logo);
    ui_draw_text(lcd, (uint16_t)(c->x + (c->w - tw) / 2), (uint16_t)(c->y + 22),
                 logo, WIKI_C_TEXT, WIKI_C_BG);
    ui_draw_hline(lcd, (uint16_t)(c->x + 40), (uint16_t)(c->y + 44),
                  (uint16_t)(c->w - 80), WIKI_C_BORDER);

    ui_rect_t f = wiki_search_field(c);
    ui_draw_fill_rect(lcd, &f, WIKI_C_BG);
    ui_draw_outline(lcd, &f, WIKI_C_TEXT);

    char shown[WIKI_MAX_QUERY];
    memcpy(shown, st->query, (size_t)st->query_cursor);
    shown[st->query_cursor] = '\0';
    ui_draw_text(lcd, (uint16_t)(f.x + 6), (uint16_t)(f.y + 10), shown,
                 WIKI_C_TEXT, WIKI_C_BG);

    uint16_t caret_x = f.x + 6 + ui_text_width(shown);
    ui_rect_t caret = { caret_x, (uint16_t)(f.y + 10), 2, CHAR_H };
    ui_draw_fill_rect(lcd, &caret, WIKI_C_TEXT);

    ui_rect_t b = wiki_search_button(c);
    ui_draw_fill_rect(lcd, &b, WIKI_C_BTN);
    ui_draw_outline(lcd, &b, WIKI_C_BTN_BRD);
    const char *lbl = "Search";
    uint16_t bw = ui_text_width(lbl);
    ui_draw_text(lcd, (uint16_t)(b.x + (b.w - bw) / 2), (uint16_t)(b.y + 10),
                 lbl, WIKI_C_BG, WIKI_C_BTN);

    const char *hint = "Type a topic - Enter or Search";
    tw = ui_text_width(hint);
    ui_draw_text(lcd, (uint16_t)(c->x + (c->w - tw) / 2), (uint16_t)(c->y + c->h - 26),
                 hint, WIKI_C_DIM, WIKI_C_BG);
}

static void wiki_draw_results(ili9488_t *lcd, const ui_rect_t *c, wiki_state_t *st)
{
    ui_draw_fill_rect(lcd, c, WIKI_C_BG);

    char head[WIKI_MAX_QUERY + 24];
    snprintf(head, sizeof(head), "Results: %.*s (%d)",
             WIKI_MAX_QUERY, st->query, st->num_results);
    ui_draw_text(lcd, (uint16_t)(c->x + 6), (uint16_t)(c->y + 4), head,
                 WIKI_C_TEXT, WIKI_C_BG);
    ui_draw_hline(lcd, c->x, (uint16_t)(c->y + 22), c->w, WIKI_C_BORDER);

    int rows = wiki_rows_visible(c);
    if (st->scroll_results > st->num_results - rows) st->scroll_results = st->num_results - rows;
    if (st->scroll_results < 0) st->scroll_results = 0;
    if (st->sel_result < st->scroll_results) st->scroll_results = st->sel_result;
    if (st->sel_result >= st->scroll_results + rows) st->scroll_results = st->sel_result - rows + 1;

    for (int i = 0; i < rows; i++) {
        int idx = st->scroll_results + i;
        if (idx >= st->num_results) break;
        wiki_result_t *r = &st->results[idx];
        int ry = c->y + 28 + i * 38;
        bool selrow = (idx == st->sel_result);

        if (selrow) {
            ui_rect_t hl = { (uint16_t)(c->x + 2), (uint16_t)(ry - 2),
                             (uint16_t)(c->w - 4), 38 };
            ui_draw_fill_rect(lcd, &hl, WIKI_C_GRAY_BG);
            ui_rect_t bar = { (uint16_t)(c->x + 2), (uint16_t)(ry - 2), 3, 38 };
            ui_draw_fill_rect(lcd, &bar, WIKI_C_BTN);
        }

        char title[WIKI_MAX_TITLE + 16];
        snprintf(title, sizeof(title), "%d. %s", idx + 1, r->title);
        ui_draw_text(lcd, (uint16_t)(c->x + 10), (uint16_t)ry, title,
                     selrow ? WIKI_C_BTN_BRD : WIKI_C_TEXT, WIKI_C_BG);

        char snip[WIKI_MAX_SNIPPET];
        strncpy(snip, r->snippet, sizeof(snip) - 1);
        snip[sizeof(snip) - 1] = '\0';
        /* trim snippet to fit one line */
        int max_px = c->w - 24;
        int acc = 0, cut = 0;
        while (snip[cut] && acc + CHAR_W <= max_px) { acc += CHAR_W; cut++; }
        snip[cut] = '\0';
        ui_draw_text(lcd, (uint16_t)(c->x + 18), (uint16_t)(ry + CHAR_H + 2), snip,
                     WIKI_C_DIM, WIKI_C_BG);
    }

    if (st->num_results == 0) {
        ui_draw_text(lcd, (uint16_t)(c->x + 10), (uint16_t)(c->y + 40),
                     "No results.", WIKI_C_DIM, WIKI_C_BG);
    }

    const char *hint = "Up/Dn select  Enter open  Esc back";
    uint16_t tw = ui_text_width(hint);
    ui_draw_text(lcd, (uint16_t)(c->x + (c->w - tw) / 2), (uint16_t)(c->y + c->h - 14),
                 hint, WIKI_C_DIM, WIKI_C_BG);
}

static void wiki_draw_article(ili9488_t *lcd, const ui_rect_t *c, wiki_state_t *st)
{
    ui_draw_fill_rect(lcd, c, WIKI_C_BG);

    /* article title strip */
    char ttl[WIKI_MAX_TITLE];
    strncpy(ttl, st->article_title, sizeof(ttl) - 1);
    ttl[sizeof(ttl) - 1] = '\0';
    int max_px = c->w - 16;
    int acc = 0, cut = 0;
    while (ttl[cut] && acc + CHAR_W <= max_px) { acc += CHAR_W; cut++; }
    ttl[cut] = '\0';
    ui_draw_text(lcd, (uint16_t)(c->x + 6), (uint16_t)(c->y + 3), ttl,
                 WIKI_C_TEXT, WIKI_C_BG);
    ui_draw_hline(lcd, c->x, (uint16_t)(c->y + WIKI_TITLE_BAR_H - 2), c->w, WIKI_C_BORDER);

    ui_rect_t area = wiki_article_area(c);
    if (st->html)
        wiki_render_html(lcd, &area, st, st->article_scroll);

    /* scrollbar */
    int total = st->article_total_lines;
    int vis = area.h / WIKI_SCROLL_LINE_H;
    if (vis < 1) vis = 1;
    if (total > vis) {
        int track_y = area.y;
        int track_h = area.h;
        ui_rect_t trk = { (uint16_t)(c->x + c->w - 5), (uint16_t)track_y, 3, (uint16_t)track_h };
        ui_draw_fill_rect(lcd, &trk, WIKI_C_GRAY_BG);
        int thumb_h = track_h * vis / total;
        if (thumb_h < 8) thumb_h = 8;
        int max_off = total - vis;
        int ty = track_y;
        if (max_off > 0)
            ty = track_y + (track_h - thumb_h) * st->article_scroll / max_off;
        ui_rect_t th = { (uint16_t)(c->x + c->w - 5), (uint16_t)ty, 3, (uint16_t)thumb_h };
        ui_draw_fill_rect(lcd, &th, WIKI_C_DIM);
    }

    const char *hint = "Arrows/PgUp/PgDn scroll  click link  Esc back";
    uint16_t tw = ui_text_width(hint);
    ui_draw_text(lcd, (uint16_t)(c->x + (c->w - tw) / 2), (uint16_t)(c->y + c->h - 13),
                 hint, WIKI_C_DIM, WIKI_C_BG);
}

static void wiki_draw_error(ili9488_t *lcd, const ui_rect_t *c, wiki_state_t *st)
{
    ui_draw_fill_rect(lcd, c, WIKI_C_BG);
    ui_draw_text(lcd, (uint16_t)(c->x + 10), (uint16_t)(c->y + 30),
                 "Error", WIKI_C_BTN_BRD, WIKI_C_BG);
    ui_draw_text(lcd, (uint16_t)(c->x + 10), (uint16_t)(c->y + 54),
                 st->error_msg, WIKI_C_TEXT, WIKI_C_BG);
    const char *hint = "Esc: back";
    uint16_t tw = ui_text_width(hint);
    ui_draw_text(lcd, (uint16_t)(c->x + (c->w - tw) / 2), (uint16_t)(c->y + c->h - 26),
                 hint, WIKI_C_DIM, WIKI_C_BG);
}

static void wiki_draw_help(ili9488_t *lcd, const ui_rect_t *c)
{
    ui_rect_t h = { (uint16_t)(c->x + 24), (uint16_t)(c->y + 6),
                    (uint16_t)(c->w - 48), (uint16_t)(c->h - 12) };
    ui_draw_fill_rect(lcd, &h, WIKI_C_BORDER);
    ui_draw_outline(lcd, &h, WIKI_C_BG);

    const char *lines[] = {
        "Wiki shortcuts",
        "Search: type, Enter submits",
        "Results: Up/Dn select, Enter open",
        "Article: arrows scroll lines",
        "  PgUp/PgDn pages, Home/End ends",
        "  wheel scrolls, click follows link",
        "Esc back   F7 help   F10 fullscreen",
    };
    int y = h.y + 6;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++, y += CHAR_H + 3) {
        if (y + CHAR_H > h.y + h.h) break;
        ui_draw_text(lcd, (uint16_t)(h.x + 8), (uint16_t)y, lines[i],
                     i == 0 ? WIKI_C_BG : WIKI_C_GRAY_BG, WIKI_C_BORDER);
    }
}

/* ------------------------------------------------------------------ */
/* input                                                               */
/* ------------------------------------------------------------------ */

static void wiki_event(arpile_app_ctx_t *ctx, const arpile_input_event_t *ev)
{
    wiki_state_t *st = ctx->user;
    if (!st) return;
    bool dirty = false;

    if (ev->type == ARPILE_IN_EVENT_KEY_DOWN) {
        if (ev->key.keycode == ARPILE_KEY_F7) {
            st->help_open = !st->help_open;
            arpile_ui_win_redraw(ctx->win);
            return;
        }
        /* F10 fullscreen handled by the window manager */

        if (st->help_open) {
            if (ev->key.keycode == ARPILE_KEY_ESCAPE) {
                st->help_open = false;
                arpile_ui_win_redraw(ctx->win);
            }
            return;
        }

        switch (st->view) {
        case WIKI_VIEW_SEARCH: {
            uint16_t kc = ev->key.keycode;
            if (kc == ARPILE_KEY_ENTER) {
                wiki_do_search(st);
            } else if (kc == ARPILE_KEY_ESCAPE) {
                arpile_app_close(ctx);
                return;
            } else if (kc == ARPILE_KEY_BACKSPACE || ev->key.ascii == 8) {
                if (st->query_cursor > 0) {
                    memmove(st->query + st->query_cursor - 1,
                            st->query + st->query_cursor,
                            (size_t)(st->query_len - st->query_cursor) + 1);
                    st->query_cursor--;
                    st->query_len--;
                    dirty = true;
                }
            } else if (kc == ARPILE_KEY_LEFT) {
                if (st->query_cursor > 0) { st->query_cursor--; dirty = true; }
            } else if (kc == ARPILE_KEY_RIGHT) {
                if (st->query_cursor < st->query_len) { st->query_cursor++; dirty = true; }
            } else if (ev->key.ascii >= 32 && ev->key.ascii < 127) {
                if (st->query_len < WIKI_MAX_QUERY - 1) {
                    memmove(st->query + st->query_cursor + 1,
                            st->query + st->query_cursor,
                            (size_t)(st->query_len - st->query_cursor));
                    st->query[st->query_cursor++] = ev->key.ascii;
                    st->query_len++;
                    st->query[st->query_len] = '\0';
                    dirty = true;
                }
            }
            break;
        }
        case WIKI_VIEW_RESULTS: {
            ui_rect_t rc = win_client_rect(ctx->win);
            int rows = wiki_rows_visible(&rc);
            switch (ev->key.keycode) {
            case ARPILE_KEY_UP:
                if (st->sel_result > 0) st->sel_result--;
                dirty = true;
                break;
            case ARPILE_KEY_DOWN:
                if (st->sel_result < st->num_results - 1) st->sel_result++;
                dirty = true;
                break;
            case ARPILE_KEY_PGUP:
                st->sel_result -= rows;
                if (st->sel_result < 0) st->sel_result = 0;
                dirty = true;
                break;
            case ARPILE_KEY_PGDN:
                st->sel_result += rows;
                if (st->sel_result >= st->num_results) st->sel_result = st->num_results - 1;
                dirty = true;
                break;
            case ARPILE_KEY_ENTER:
                wiki_open_result(st, st->sel_result);
                dirty = true;
                break;
            case ARPILE_KEY_ESCAPE:
                wiki_go_back(st);
                dirty = true;
                break;
            default:
                break;
            }
            break;
        }
        case WIKI_VIEW_ARTICLE: {
            ui_rect_t c = win_client_rect(ctx->win);
            ui_rect_t area = wiki_article_area(&c);
            int vis = area.h / WIKI_SCROLL_LINE_H;
            if (vis < 1) vis = 1;
            switch (ev->key.keycode) {
            case ARPILE_KEY_UP:    st->article_scroll -= 1; dirty = true; break;
            case ARPILE_KEY_DOWN:  st->article_scroll += 1; dirty = true; break;
            case ARPILE_KEY_PGUP:  st->article_scroll -= vis; dirty = true; break;
            case ARPILE_KEY_PGDN:  st->article_scroll += vis; dirty = true; break;
            case ARPILE_KEY_HOME:  st->article_scroll = 0; dirty = true; break;
            case ARPILE_KEY_END:   st->article_scroll = st->article_total_lines; dirty = true; break;
            case ARPILE_KEY_ESCAPE:
                wiki_go_back(st);
                dirty = true;
                break;
            default:
                break;
            }
            if (dirty) {
                int max_scroll = st->article_total_lines - vis;
                if (max_scroll < 0) max_scroll = 0;
                if (st->article_scroll < 0) st->article_scroll = 0;
                if (st->article_scroll > max_scroll) st->article_scroll = max_scroll;
            }
            break;
        }
        case WIKI_VIEW_ERROR:
            if (ev->key.keycode == ARPILE_KEY_ESCAPE || ev->key.keycode == ARPILE_KEY_ENTER) {
                wiki_go_back(st);
                dirty = true;
            }
            break;
        }

        if (dirty) arpile_ui_win_redraw(ctx->win);
        return;
    }

    if (ev->type == ARPILE_IN_EVENT_MOUSE_BTN &&
        (ev->mouse.buttons & ARPILE_MOUSE_BTN_LEFT)) {
        ui_rect_t c = win_client_rect(ctx->win);
        int mx = ev->mouse.x, my = ev->mouse.y;

        if (st->view == WIKI_VIEW_SEARCH) {
            ui_rect_t b = wiki_search_button(&c);
            ui_rect_t f = wiki_search_field(&c);
            if (ui_rect_contains(&f, mx, my)) {
                /* focus field: clamp caret to click position */
                int rel = mx - (f.x + 6);
                int pos = rel < 0 ? 0 : rel / CHAR_W;
                if (pos > st->query_len) pos = st->query_len;
                st->query_cursor = pos;
                arpile_ui_win_redraw(ctx->win);
            } else if (ui_rect_contains(&b, mx, my)) {
                wiki_do_search(st);
                arpile_ui_win_redraw(ctx->win);
            }
        } else if (st->view == WIKI_VIEW_RESULTS) {
            int rows = wiki_rows_visible(&c);
            int ry0 = c.y + 28;
            if (my >= ry0) {
                int row = (my - ry0) / 38;
                if (row >= 0 && row < rows) {
                    int idx = st->scroll_results + row;
                    if (idx < st->num_results) {
                        if (idx == st->sel_result) {
                            wiki_open_result(st, idx);      /* double-purpose: click again opens */
                        } else {
                            st->sel_result = idx;
                        }
                        arpile_ui_win_redraw(ctx->win);
                    }
                }
            }
        } else if (st->view == WIKI_VIEW_ARTICLE) {
            for (int i = 0; i < st->num_links; i++) {
                if (ui_rect_contains(&st->links[i].rect, mx, my)) {
                    char title[WIKI_MAX_TITLE];
                    strncpy(title, st->links[i].title, WIKI_MAX_TITLE - 1);
                    title[WIKI_MAX_TITLE - 1] = '\0';
                    wiki_push_hist(st, st->article_title, st->article_scroll);
                    st->pending_scroll_valid = 0;
                    wiki_fetch_article(st, title);
                    arpile_ui_win_redraw(ctx->win);
                    break;
                }
            }
        }
        return;
    }

    if (ev->type == ARPILE_IN_EVENT_MOUSE_WHEEL) {
        if (st->view == WIKI_VIEW_ARTICLE) {
            st->article_scroll += ev->wheel * 2;
            ui_rect_t c = win_client_rect(ctx->win);
            ui_rect_t area = wiki_article_area(&c);
            int vis = area.h / WIKI_SCROLL_LINE_H;
            int max_scroll = st->article_total_lines - vis;
            if (max_scroll < 0) max_scroll = 0;
            if (st->article_scroll < 0) st->article_scroll = 0;
            if (st->article_scroll > max_scroll) st->article_scroll = max_scroll;
            arpile_ui_win_redraw(ctx->win);
        } else if (st->view == WIKI_VIEW_RESULTS) {
            ui_rect_t rc = win_client_rect(ctx->win);
            int rows = wiki_rows_visible(&rc);
            st->scroll_results -= ev->wheel;
            int maxs = st->num_results - rows;
            if (maxs < 0) maxs = 0;
            if (st->scroll_results > maxs) st->scroll_results = maxs;
            if (st->scroll_results < 0) st->scroll_results = 0;
            arpile_ui_win_redraw(ctx->win);
        }
        return;
    }
}

/* ------------------------------------------------------------------ */
/* app lifecycle                                                       */
/* ------------------------------------------------------------------ */

static uint32_t s_seen_epoch;

static void wiki_update(arpile_app_ctx_t *ctx)
{
    wiki_state_t *st = ctx->user;
    if (!st) return;
    arpile_wifi_poll();

    /* repaint when worker tasks changed something (results arrived,
     * article installed, image decoded...) */
    if (st->dirty_epoch != s_seen_epoch) {
        s_seen_epoch = st->dirty_epoch;
        arpile_ui_win_redraw(ctx->win);
    }
}

static void wiki_render(arpile_app_ctx_t *ctx, ui_win_t *win)
{
    wiki_state_t *st = ctx->user;
    if (!st) return;
    ili9488_t *lcd = arpile_ui_get_lcd();
    ui_rect_t c = win_client_rect(win);

    /* keep our epoch in sync so update() doesn't loop-redraw */
    s_seen_epoch = st->dirty_epoch;

    switch (st->view) {
    case WIKI_VIEW_SEARCH:   wiki_draw_search(lcd, &c, st);  break;
    case WIKI_VIEW_RESULTS:  wiki_draw_results(lcd, &c, st); break;
    case WIKI_VIEW_ARTICLE:  wiki_draw_article(lcd, &c, st); break;
    case WIKI_VIEW_ERROR:    wiki_draw_error(lcd, &c, st);   break;
    }

    if (st->help_open) wiki_draw_help(lcd, &c);
}

static void wiki_init(arpile_app_ctx_t *ctx)
{
    wiki_state_t *st = calloc(1, sizeof(wiki_state_t));
    if (!st) return;
    ctx->user = st;
    g_wiki = st;
    st->view = WIKI_VIEW_SEARCH;
    st->query[0] = '\0';
    s_seen_epoch = 0;
}

static void wiki_destroy(arpile_app_ctx_t *ctx)
{
    wiki_state_t *st = ctx->user;
    if (g_wiki == st) g_wiki = NULL;
    if (!st) return;

    bool defer = false;
    portENTER_CRITICAL(&g_wiki_mux);
    st->shutdown = true;
    if (st->workers > 0) defer = true;           /* last worker frees */
    portEXIT_CRITICAL(&g_wiki_mux);

    if (!defer) wiki_free_all(st);
    ctx->user = NULL;
}

static const arpile_app_ops_t wiki_ops = {
    .init = wiki_init,
    .update = wiki_update,
    .event = wiki_event,
    .render = wiki_render,
    .destroy = wiki_destroy,
};

const arpile_app_t arpile_app_wiki = {
    .id = "wiki",
    .name = "Wiki",
    .icon = "wiki",
    .ops = &wiki_ops,
};
