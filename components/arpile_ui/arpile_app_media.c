/* Media ACEXE - indexed media library for the 480x320 desktop.
 *
 * NOT a file browser: a recursive SD scan builds /sdcard/Arpile/.media/
 * index.db (atomic tmp+rename replace; new/changed/gone reported). The
 * library runs off the index with categories, live search, sorting and
 * a preview pane.
 *
 * Hardware pipelines ONLY - there is NO software decoder fallback:
 *  - Images: JPEG via the ESP32-P4 JPEG engine and 24-bit BMP.
 *  - Video: raw .mjpg streams, AVI/MJPG and JPEG-in-MP4, decoded per
 *    frame on the JPEG engine and scaled through the PPA into RGB565.
 *    Other codecs are inspected and rejected with an explicit message
 *    naming the codec fourcc.
 *  - Audio: PCM/WAV resampled into the ES8311 stack (16 kHz stereo).
 */

#include "arpile_ui.h"
#include "arpile_app.h"
#include "arpile_imgdec.h"
#include "es8311_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "driver/jpeg_decode.h"

#define MED_SD_ROOT     "/sdcard"
#define MED_BASE        "/sdcard/Arpile"
#define MED_IDX_DIR     "/sdcard/Arpile/.media"
#define MED_IDX_DB      "/sdcard/Arpile/.media/index.db"

#define MED_PATH_MAX    176
#define MED_MAX_ITEMS   1024

#define MED_T_VIDEO 0
#define MED_T_IMAGE 1
#define MED_T_AUDIO 2

enum { MD_LIB = 0, MD_IMG, MD_VID, MD_AUD };

typedef struct {
    char path[MED_PATH_MAX];
    uint32_t size;
    long long mtime;
    uint8_t type;
} med_item_t;

/* ---- audio converter (any PCM -> 16 kHz stereo s16) --------------- */

typedef struct {
    uint32_t rate;
    uint16_t ch;
    uint16_t bits;
    uint64_t in_base;
    uint64_t out_count;
} med_cvt_t;

static void med_cvt_init(med_cvt_t *cv, uint32_t rate, uint16_t ch,
                         uint16_t bits)
{
    memset(cv, 0, sizeof(*cv));
    cv->rate = rate ? rate : 16000;
    cv->ch = ch ? ch : 1;
    cv->bits = bits;
}

static void med_cvt_feed(med_cvt_t *cv, const uint8_t *data, size_t len,
                         int16_t *out, int max_pairs)
{
    if (cv->bits != 16 && cv->bits != 8) return;
    int bps = cv->bits == 16 ? 2 : 1;
    int fsize = (int)cv->ch * bps;
    if (fsize <= 0 || len < (size_t)fsize) return;
    size_t n_in = len / (size_t)fsize;
    if (!n_in) return;

    uint64_t total_out =
        ((cv->in_base + n_in) * 16000ULL + cv->rate - 1) / cv->rate;
    uint64_t n_out64 = total_out - cv->out_count;
    size_t n_out = n_out64 > 4096 ? 4096 : (size_t)n_out64;
    if ((int)n_out > max_pairs) n_out = (size_t)max_pairs;
    for (size_t o = 0; o < n_out; o++) {
        uint64_t gi = (cv->out_count + o) * cv->rate / 16000ULL;
        uint64_t li = gi - cv->in_base;
        if (li >= n_in) li = n_in - 1;
        const uint8_t *f = data + (size_t)li * fsize;
        int l, r;
        if (cv->bits == 16) {
            l = (int)(int16_t)((uint16_t)f[0] | ((uint16_t)f[1] << 8));
            if (cv->ch >= 2)
                r = (int)(int16_t)((uint16_t)f[2] | ((uint16_t)f[3] << 8));
            else r = l;
        } else {
            l = ((int)f[0] - 128) << 8;
            if (cv->ch >= 2) r = ((int)f[1] - 128) << 8;
            else r = l;
        }
        out[o * 2] = (int16_t)l;
        out[o * 2 + 1] = (int16_t)r;
    }
    arpile_audio_write_pcm(out, n_out);
    cv->in_base += n_in;
    cv->out_count += n_out;
}

static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* ---- video source ------------------------------------------------- */

enum { VS_NONE = 0, VS_RAW, VS_AVI, VS_MP4 };

typedef struct {
    FILE *f;
    long fsize;
    int kind;

    long pos;                    /* raw/avi cursor */
    uint32_t samp;               /* mp4 sample cursor */
    int skip_next;

    long data_off;               /* body offset of current chunk */
    long frame_start;            /* raw: start of last returned frame */
    uint32_t vid_total;          /* avi: total video chunks */
    uint32_t dur_ts, dur_dur;    /* mp4 mvhd timescale/duration */
    uint32_t trak_ts;            /* mp4 mdhd timescale of video track */
    uint32_t stts_n;
    uint32_t (*stts)[2];

    /* AVI */
    long movi_start, movi_end;
    uint32_t fps_x1000;
    bool aud_pcm;
    uint16_t aud_ch;
    uint32_t aud_rate;
    uint16_t aud_bits;

    /* MP4 sample tables of the 'vide'/jpeg track */
    uint32_t stsz_def, stsz_n;
    uint32_t *stsz;
    uint32_t stco_n;
    uint32_t *stco;
    uint32_t stsc_n;
    uint32_t (*stsc)[3];
    char codec[5];

    /* decode state */
    uint8_t *jpg;
    size_t jpg_cap;
    uint8_t *raw;
    uint32_t dec_w, dec_h;

    /* presentation buffers (DMA) */
    uint16_t *fb[2];
    int fb_w, fb_h;
} vidsrc_t;

/* ---- app state ---------------------------------------------------- */

typedef struct {
    volatile uint32_t dirty;
    volatile int workers;
    volatile bool shutdown;

    med_item_t *items;
    int n_items;
    volatile int epoch;          /* bumped on scan swap */

    int cat;                     /* -1 all, else MED_T_* */
    int sort;                    /* 0 name, 1 newest, 2 size */
    char query[40];
    bool query_edit;
    int cursor;
    int top;

    volatile bool scanning;
    volatile uint32_t scan_seen;
    char scan_dir[56];
    int scan_new, scan_gone, scan_chg;

    int prev_idx;
    volatile int prev_state;     /* 1 loading, 2 ready, 3 unavailable */
    uint16_t *prev_pix;
    int prev_w, prev_h;

    volatile int mode;
    char note[80];

    /* image viewer */
    uint16_t *vpix;
    int vw, vh;
    bool vfit;
    int vpanx, vpany;

    /* video runtime */
    volatile bool vid_busy;
    volatile bool vid_pause;
    volatile bool vid_stop;
    volatile int vid_seek_pct;   /* -1 none, else 0..100 */
    volatile int vid_pct;
    volatile uint32_t vid_frame;
    uint16_t *volatile vid_fbptr;
    volatile int vid_dx, vid_dy, vid_dw, vid_dh;
    volatile uint32_t vid_seq;
    void *vid_job;               /* heap vidjob, freed on UI task */

    char open_path[MED_PATH_MAX];/* target handed to a freshly spawned player */
    volatile int vstate;         /* image viewer: 1 loading, 2 ready, 3 failed */

    /* audio runtime */
    volatile bool aud_busy;
    volatile bool aud_stop;
    volatile int aud_pct;
    char aud_name[36];

    bool nosd;
} med_t;

static portMUX_TYPE g_med_mux = portMUX_INITIALIZER_UNLOCKED;

static void med_touch(med_t *m) { m->dirty++; }

static void med_worker_exit(med_t *m)
{
    portENTER_CRITICAL(&g_med_mux);
    m->workers--;
    bool last_out = (m->shutdown && m->workers == 0);
    portEXIT_CRITICAL(&g_med_mux);
    if (last_out) {
        free(m->items);
        free(m->prev_pix);
        free(m->vpix);
        free(m->vid_job);
        free(m);
    }
    vTaskDelete(NULL);
}

static void med_spawn(med_t *m, TaskFunction_t fn, const char *name,
                      int stack, int prio)
{
    bool spawn = false;
    portENTER_CRITICAL(&g_med_mux);
    if (!m->shutdown) {
        m->workers++;
        spawn = true;
    }
    portEXIT_CRITICAL(&g_med_mux);
    if (spawn &&
        xTaskCreate(fn, name, (uint16_t)stack, m, (UBaseType_t)prio,
                    NULL) != pdPASS) {
        portENTER_CRITICAL(&g_med_mux);
        m->workers--;
        portEXIT_CRITICAL(&g_med_mux);
    }
}

/* ------------------------------------------------------------------ */
/* classification + small helpers                                      */
/* ------------------------------------------------------------------ */

static const char *med_ext(const char *path)
{
    const char *s = strrchr(path, '.');
    return s ? s + 1 : "";
}

static bool med_ext_is(const char *path, const char *const *list)
{
    const char *e = med_ext(path);
    for (int i = 0; list[i]; i++)
        if (!strcasecmp(e, list[i])) return true;
    return false;
}

static const char *const k_vid[] = { "mp4", "m4v", "mov", "avi",
                                     "mjpg", "mjpeg", NULL };
static const char *const k_img[] = { "jpg", "jpeg", "bmp", NULL };
static const char *const k_aud[] = { "wav", "mp3", "aac", "flac", "ogg",
                                     "m4a", "opus", NULL };

static uint8_t med_classify(const char *path)
{
    if (med_ext_is(path, k_img)) return MED_T_IMAGE;
    if (med_ext_is(path, k_vid)) return MED_T_VIDEO;
    if (med_ext_is(path, k_aud)) return MED_T_AUDIO;
    return 255;
}

static const char *med_type_icon(int t)
{
    return t == MED_T_VIDEO ? "movie" :
           t == MED_T_AUDIO ? "music_note" : "image";
}

static void med_human_size(uint32_t sz, char *out, size_t cap)
{
    if (sz >= 1024u * 1024u)
        snprintf(out, cap, "%.1fM", (double)sz / (1024.0 * 1024.0));
    else if (sz >= 1024u)
        snprintf(out, cap, "%uK", (unsigned)(sz / 1024));
    else
        snprintf(out, cap, "%uB", (unsigned)sz);
}

/* ------------------------------------------------------------------ */
/* index database                                                      */
/* ------------------------------------------------------------------ */

static void med_load_db(med_t *m)
{
    FILE *f = fopen(MED_IDX_DB, "r");
    if (!f) return;
    char line[MED_PATH_MAX + 72];
    int n = 0;
    while (n < MED_MAX_ITEMS && fgets(line, sizeof(line), f)) {
        unsigned t = 255;
        unsigned long sz = 0;
        long long mt = 0;
        char path[MED_PATH_MAX];
        if (sscanf(line, "%u %lu %lld %175[^\n]", &t, &sz, &mt, path) == 4 &&
            t <= 2 && path[0]) {
            med_item_t *it = &m->items[n++];
            snprintf(it->path, MED_PATH_MAX, "%.170s", path);
            it->size = (uint32_t)sz;
            it->mtime = mt;
            it->type = (uint8_t)t;
        }
    }
    fclose(f);
    m->n_items = n;
}

static void med_save_db(med_t *m)
{
    mkdir(MED_IDX_DIR, 0775);
    FILE *f = fopen(MED_IDX_DB ".tmp", "w");
    if (!f) return;
    for (int i = 0; i < m->n_items; i++) {
        const med_item_t *it = &m->items[i];
        fprintf(f, "%u %lu %lld %s\n",
                (unsigned)it->type, (unsigned long)it->size,
                it->mtime, it->path);
    }
    fclose(f);
    rename(MED_IDX_DB ".tmp", MED_IDX_DB);
}

/* ------------------------------------------------------------------ */
/* scanner                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char path[120];
} med_dirframe_t;

#define SCAN_STACK_MAX 12

static void med_scan_task(void *arg)
{
    med_t *m = (med_t *)arg;

    med_dirframe_t *stack = calloc(SCAN_STACK_MAX, sizeof(*stack));
    med_item_t *old = m->items;
    int old_n = m->n_items;
    uint8_t *seen = old_n > 0 ? calloc((size_t)old_n, 1) : NULL;
    med_item_t *fresh = calloc(MED_MAX_ITEMS, sizeof(med_item_t));
    int nf = 0;

    if (stack && fresh) {
        snprintf(stack[0].path, sizeof(stack[0].path), "%s", MED_SD_ROOT);
        int sp = 1;
        m->scan_new = m->scan_gone = m->scan_chg = 0;
        m->scan_seen = 0;

        while (sp > 0 && !m->shutdown) {
            sp--;
            char dir[128];
            snprintf(dir, sizeof(dir), "%.120s", stack[sp].path);
            snprintf(m->scan_dir, sizeof(m->scan_dir), "%.52s",
                     dir + strlen(MED_SD_ROOT));

            DIR *d = opendir(dir);
            if (!d) continue;
            struct dirent *e;
            while ((e = readdir(d)) != NULL && !m->shutdown) {
                if (e->d_name[0] == '.') continue;
                char full[MED_PATH_MAX];
                snprintf(full, sizeof(full), "%.164s/%.47s",
                         dir, e->d_name);

                struct stat st;
                if (stat(full, &st) != 0) continue;
                if (S_ISDIR(st.st_mode)) {
                    if (!strncmp(full, MED_BASE, strlen(MED_BASE)))
                        continue;
                    if (sp < SCAN_STACK_MAX - 1) {
                        snprintf(stack[sp].path, sizeof(stack[sp].path),
                                 "%.116s", full);
                        sp++;
                    }
                    continue;
                }
                if (!S_ISREG(st.st_mode)) continue;
                uint8_t t = med_classify(full);
                if (t == 255) continue;
                m->scan_seen++;

                if (nf >= MED_MAX_ITEMS) continue;
                med_item_t *it = &fresh[nf++];
                snprintf(it->path, MED_PATH_MAX, "%.170s", full);
                it->size = (uint32_t)st.st_size;
                it->mtime = (long long)st.st_mtime;
                it->type = t;

                bool existed = false;
                for (int i = 0; i < old_n; i++) {
                    if (!strcmp(old[i].path, it->path)) {
                        seen[i] = 1;
                        if (old[i].size != it->size ||
                            old[i].mtime != it->mtime)
                            m->scan_chg++;
                        existed = true;
                        break;
                    }
                }
                if (!existed) m->scan_new++;
            }
            closedir(d);
        }

        for (int i = 0; i < old_n; i++)
            if (!seen[i]) m->scan_gone++;

        if (!m->shutdown) {
            m->items = fresh;
            m->n_items = nf;
            m->epoch++;
            free(old);
            free(seen);
            seen = NULL;
            old = NULL;
            med_save_db(m);
            m->cursor = 0;
            m->top = 0;
            m->prev_idx = -1;
        }
    }

    free(seen);
    free(old);
    free(stack);
    free(fresh == m->items ? NULL : fresh);
    if (!m->shutdown) {
        m->scanning = false;
        med_touch(m);
    }
    med_worker_exit(m);
}

/* ------------------------------------------------------------------ */
/* file helpers + image decoding                                       */
/* ------------------------------------------------------------------ */

static bool med_read_file(const char *path, uint8_t **out, size_t *out_len,
                          size_t max_len)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || (size_t)st.st_size > max_len)
        return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t *buf = malloc((size_t)st.st_size);
    if (!buf) { fclose(f); return false; }
    size_t got = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    if (got != (size_t)st.st_size) { free(buf); return false; }
    *out = buf;
    *out_len = got;
    return true;
}

static uint16_t *med_bmp_decode(const char *path, int *out_w, int *out_h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        fclose(f);
        return NULL;
    }
    uint32_t off = (uint32_t)hdr[10] | ((uint32_t)hdr[11] << 8) |
                   ((uint32_t)hdr[12] << 16) | ((uint32_t)hdr[13] << 24);
    int32_t w = (int32_t)((uint32_t)hdr[18] | ((uint32_t)hdr[19] << 8) |
                          ((uint32_t)hdr[20] << 16) |
                          ((uint32_t)hdr[21] << 24));
    int32_t h = (int32_t)((uint32_t)hdr[22] | ((uint32_t)hdr[23] << 8) |
                          ((uint32_t)hdr[24] << 16) |
                          ((uint32_t)hdr[25] << 24));
    uint16_t bpp = (uint16_t)(hdr[28] | (hdr[29] << 8));
    if (bpp != 24 || w <= 0 || h == 0 ||
        w > 800 || (h > 0 ? h : -h) > 800) {
        fclose(f);
        return NULL;
    }
    bool flip = h > 0;
    if (h < 0) h = -h;

    uint16_t *pix = malloc((size_t)w * h * 2);
    uint8_t *row = malloc((size_t)((w * 3 + 3) & ~3));
    if (!pix || !row) { free(pix); free(row); fclose(f); return NULL; }
    int rowbytes = (w * 3 + 3) & ~3;
    fseek(f, (long)off, SEEK_SET);
    bool ok = true;
    for (int y = 0; y < h && ok; y++) {
        if (fread(row, 1, (size_t)rowbytes, f) != (size_t)rowbytes) {
            ok = false;
            break;
        }
        int dy = flip ? (h - 1 - y) : y;
        uint16_t *dst = pix + (size_t)dy * w;
        for (int x = 0; x < w; x++)
            dst[x] = UI_RGB(row[x * 3 + 2], row[x * 3 + 1], row[x * 3]);
    }
    free(row);
    fclose(f);
    if (!ok) { free(pix); return NULL; }
    *out_w = w;
    *out_h = h;
    return pix;
}

static uint16_t *med_scale_fit(const uint16_t *src, int sw, int sh,
                               int bw, int bh, int *dw, int *dh)
{
    float s = 1.0f;
    if (sw > bw) s = (float)bw / sw;
    if ((float)sh * s > bh) s = (float)bh / sh;
    int ow = (int)(sw * s);
    if (ow < 1) ow = 1;
    int oh = (int)(sh * s);
    if (oh < 1) oh = 1;
    uint16_t *dst = malloc((size_t)ow * oh * 2);
    if (!dst) return NULL;
    for (int y = 0; y < oh; y++) {
        const uint16_t *sr = src + (size_t)((uint64_t)y * sh / oh) * sw;
        uint16_t *dr = dst + (size_t)y * ow;
        for (int x = 0; x < ow; x++)
            dr[x] = sr[(uint32_t)((uint64_t)x * sw / ow)];
    }
    *dw = ow;
    *dh = oh;
    return dst;
}

static void med_preview_task(void *arg)
{
    med_t *m = (med_t *)arg;
    int idx = m->prev_idx;
    char path[MED_PATH_MAX];
    uint8_t type = 255;
    if (idx >= 0 && idx < m->n_items) {
        snprintf(path, sizeof(path), "%.170s", m->items[idx].path);
        type = m->items[idx].type;
    } else {
        path[0] = '\0';
    }

    free(m->prev_pix);
    m->prev_pix = NULL;
    m->prev_state = 1;
    med_touch(m);

    uint16_t *pix = NULL;
    int pw = 0, ph = 0;
    if (path[0] && type == MED_T_IMAGE) {
        const char *e = med_ext(path);
        if (!strcasecmp(e, "bmp")) {
            int w = 0, h = 0;
            uint16_t *full = med_bmp_decode(path, &w, &h);
            if (full) {
                pix = med_scale_fit(full, w, h, 150, 110, &pw, &ph);
                free(full);
            }
        } else {
            uint8_t *buf = NULL;
            size_t len = 0;
            if (med_read_file(path, &buf, &len, 6u * 1024 * 1024)) {
                if (buf[0] == 0xFF && buf[1] == 0xD8)
                    pix = arpile_jpeg_decode_hw(buf, len, 150, 110,
                                                1024, 1024, &pw, &ph);
                free(buf);
            }
        }
    }

    if (m->shutdown) {
        free(pix);
    } else if (m->prev_idx != idx) {
        /* user scrolled elsewhere while decoding: discard, re-arm */
        free(pix);
        m->prev_state = 0;
        m->prev_idx = -1;
        med_touch(m);
    } else {
        m->prev_pix = pix;
        m->prev_w = pw;
        m->prev_h = ph;
        m->prev_state = pix ? 2 : 3;
        med_touch(m);
    }
    med_worker_exit(m);
}

/* ------------------------------------------------------------------ */
/* filtered/sorted view                                                */
/* ------------------------------------------------------------------ */

static bool med_ci_str(const char *h, const char *n)
{
    if (!*n) return true;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b &&
               tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*b) return true;
    }
    return false;
}

static bool med_visible(const med_t *m, const med_item_t *it)
{
    if (m->cat >= 0 && it->type != m->cat) return false;
    if (m->query[0] && !med_ci_str(it->path, m->query)) return false;
    return true;
}

static int med_count_visible(const med_t *m)
{
    int c = 0;
    for (int i = 0; i < m->n_items; i++)
        if (med_visible(m, &m->items[i])) c++;
    return c;
}

static int med_pos_to_idx(const med_t *m, int pos)
{
    static int map[MED_MAX_ITEMS];
    static int map_n = -1;
    static int map_src_n = -1, map_epoch = -1;
    static int map_cat = -999, map_sort = -1;
    static char map_q[40];

    if (map_n < 0 || map_src_n != m->n_items || map_epoch != m->epoch ||
        map_cat != m->cat || map_sort != m->sort ||
        strcmp(map_q, m->query) != 0) {
        map_n = 0;
        for (int i = 0; i < m->n_items; i++)
            if (med_visible(m, &m->items[i])) map[map_n++] = i;
        for (int a = 1; a < map_n; a++) {
            int v = map[a];
            int b = a - 1;
            while (b >= 0) {
                const med_item_t *x = &m->items[map[b]];
                const med_item_t *y = &m->items[v];
                bool swap = false;
                if (m->sort == 1) swap = y->mtime > x->mtime;
                else if (m->sort == 2) swap = y->size > x->size;
                else swap = strcasecmp(x->path, y->path) > 0;
                if (!swap) break;
                map[b + 1] = map[b];
                b--;
            }
            map[b + 1] = v;
        }
        map_src_n = m->n_items;
        map_epoch = m->epoch;
        map_cat = m->cat;
        map_sort = m->sort;
        snprintf(map_q, sizeof(map_q), "%s", m->query);
    }
    return pos >= 0 && pos < map_n ? map[pos] : -1;
}
/* ------------------------------------------------------------------ */
/* AVI demuxer                                                         */
/* ------------------------------------------------------------------ */

static bool vid_grow_jpg(vidsrc_t *vs, size_t need)
{
    if (need <= vs->jpg_cap) return true;
    size_t nc = vs->jpg_cap ? vs->jpg_cap : 262144;
    while (nc < need) nc *= 2;
    if (nc > 3u * 1024 * 1024) return false;
    uint8_t *nb = realloc(vs->jpg, nc);
    if (!nb) return false;
    vs->jpg = nb;
    vs->jpg_cap = nc;
    return true;
}

/* concatenated JPEG frames; resyncs on the next SOI anywhere */
static long vid_raw_next(vidsrc_t *vs)
{
    if (fseek(vs->f, vs->pos, SEEK_SET) != 0) return 0;
    uint8_t buf[1024];
    int phase = 0;               /* 0 scan SOI, 1 collect frame */
    int st = 0;                  /* FFD8FF match state */
    size_t fill = 0;
    long abso = vs->pos;
    long fstart = 0;

    for (;;) {
        size_t got = fread(buf, 1, sizeof(buf), vs->f);
        if (!got) return 0;
        size_t i = 0;
        while (i < got) {
            if (phase == 0) {
                uint8_t c = buf[i];
                if (st == 0) st = (c == 0xFF) ? 1 : 0;
                else if (st == 1) st = (c == 0xD8) ? 2 :
                                      (c == 0xFF ? 1 : 0);
                else if (st == 2) st = (c == 0xFF) ? 3 : 0;
                else if (st == 3) {
                    phase = 1;
                    fill = 0;
                    i -= 3;      /* collector re-reads FF D8 FF */
                    fstart = abso + (long)i;
                    continue;
                }
                i++;
                continue;
            }
            if (!vid_grow_jpg(vs, fill + 1)) return 0;
            vs->jpg[fill++] = buf[i++];
            if (fill >= 4 && vs->jpg[fill - 2] == 0xFF &&
                vs->jpg[fill - 1] == 0xD9) {
                vs->data_off = 0;
                vs->frame_start = fstart;
                vs->pos = abso + (long)i;
                return (long)fill;
            }
        }
        abso += (long)got;
    }
}

static bool vid_avi_scan(vidsrc_t *vs)
{
    FILE *f = vs->f;
    uint8_t h[12];

    if (fseek(f, 0, SEEK_SET) != 0 || fread(h, 1, 12, f) != 12 ||
        memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "AVI ", 4) != 0)
        return false;

    long off = 12;
    uint32_t scale = 0, rate = 0;
    uint16_t fmt_tag = 0;

    while (off + 8 <= vs->fsize) {
        if (fseek(f, off, SEEK_SET) != 0 || fread(h, 1, 8, f) != 8) break;
        uint32_t sz = rd32le(h + 4);
        long body = off + 8;
        if (!memcmp(h, "LIST", 4)) {
            uint8_t lt[4];
            if (fread(lt, 1, 4, f) != 4) break;
            if (!memcmp(lt, "hdrl", 4)) {
                long so = body + 4;
                long send = body + (long)sz;
                while (so + 8 <= send) {
                    uint8_t sh[8];
                    if (fseek(f, so, SEEK_SET) != 0 ||
                        fread(sh, 1, 8, f) != 8) break;
                    uint32_t ssz = rd32le(sh + 4);
                    if (!memcmp(sh, "strl", 4)) {
                        long to = so + 8;
                        long tend = so + 8 + (long)ssz;
                        int is_vid = -1;
                        while (to + 8 <= tend) {
                            uint8_t th[8];
                            if (fseek(f, to, SEEK_SET) != 0 ||
                                fread(th, 1, 8, f) != 8) break;
                            uint32_t tsz = rd32le(th + 4);
                            long tbody = to + 8;
                            if (!memcmp(th, "strh", 4) && tsz >= 48) {
                                uint8_t sb[48];
                                if (fseek(f, tbody, SEEK_SET) == 0 &&
                                    fread(sb, 1, 48, f) == 48) {
                                    if (!memcmp(sb, "vids", 4)) {
                                        is_vid = 1;
                                        scale = rd32le(sb + 20);
                                        rate = rd32le(sb + 24);
                                    } else if (!memcmp(sb, "auds", 4)) {
                                        is_vid = 0;
                                    }
                                }
                            } else if (!memcmp(th, "strf", 4) &&
                                       tsz >= 40 && is_vid >= 0) {
                                uint8_t sb[40];
                                if (fseek(f, tbody, SEEK_SET) == 0 &&
                                    fread(sb, 1, 40, f) == 40) {
                                    if (is_vid == 1) {
                                        memcpy(vs->codec, sb + 16, 4);
                                    } else {
                                        fmt_tag = rd16le(sb);
                                        vs->aud_ch = rd16le(sb + 2);
                                        vs->aud_rate = rd32le(sb + 4);
                                        vs->aud_bits = rd16le(sb + 16);
                                    }
                                }
                            }
                            to = tbody + (long)((tsz + 1) & ~1u);
                        }
                    }
                    so += 8 + (long)((ssz + 1) & ~1u);
                }
            } else if (!memcmp(lt, "movi", 4)) {
                vs->movi_start = body + 4;
                vs->movi_end = body + (long)sz;
            }
        }
        if (vs->movi_start) break;
        off = body + (long)((sz + 1) & ~1u);
    }

    if (!vs->movi_start || !vs->movi_end) return false;
    vs->aud_pcm = (fmt_tag == 1);
    bool jpeg_codec = !memcmp(vs->codec, "MJPG", 4) ||
                      !memcmp(vs->codec, "jpeg", 4) ||
                      !memcmp(vs->codec, "JPEG", 4);
    if (!jpeg_codec) return false;

    vs->fps_x1000 = (scale && rate) ? rate * 1000u / scale : 10000;
    if (vs->fps_x1000 < 100) vs->fps_x1000 = 100;
    if (vs->fps_x1000 > 60000) vs->fps_x1000 = 60000;

    /* count video chunks for progress + seeking */
    long co = vs->movi_start;
    uint32_t n = 0;
    while (co + 8 <= vs->movi_end && n < 200000) {
        uint8_t ch[8];
        if (fseek(f, co, SEEK_SET) != 0 || fread(ch, 1, 8, f) != 8) break;
        uint32_t csz = rd32le(ch + 4);
        if (!memcmp(ch, "00dc", 4)) n++;
        co += 8 + (long)((csz + 1) & ~1u);
    }
    vs->vid_total = n;
    return true;
}

/* >0: video frame of that length at data_off; <0: audio chunk (-len); 0 EOF */
static long vid_avi_next(vidsrc_t *vs, bool *audio)
{
    FILE *f = vs->f;
    uint8_t h[8];

    while (vs->pos + 8 <= vs->movi_end) {
        if (fseek(f, vs->pos, SEEK_SET) != 0 ||
            fread(h, 1, 8, f) != 8) return 0;
        uint32_t sz = rd32le(h + 4);
        long body = vs->pos + 8;
        long nxt = body + (long)((sz + 1) & ~1u);
        if (nxt > vs->movi_end) return 0;
        vs->pos = nxt;
        if (!memcmp(h, "00dc", 4)) {
            vs->data_off = body;
            *audio = false;
            return (long)sz;
        }
        if (!memcmp(h, "01wb", 4) && vs->aud_pcm) {
            vs->data_off = body;
            *audio = true;
            return -(long)sz;
        }
    }
    return 0;
}

/* jump to roughly pct% through the movi list by counting video chunks */
static void vid_avi_seek_pct(vidsrc_t *vs, int pct)
{
    FILE *f = vs->f;
    long off = vs->movi_start;
    uint32_t target = (uint32_t)((uint64_t)vs->vid_total * (uint32_t)pct / 100u);
    uint32_t seen = 0;
    uint8_t h[8];

    while (off + 8 <= vs->movi_end) {
        uint8_t dummy;
        (void)dummy;
        if (fseek(f, off, SEEK_SET) != 0 || fread(h, 1, 8, f) != 8) break;
        uint32_t sz = rd32le(h + 4);
        if (!memcmp(h, "00dc", 4)) {
            if (seen >= target) break;
            seen++;
        }
        off += 8 + (long)((sz + 1) & ~1u);
    }
    vs->pos = off;
}

/* ------------------------------------------------------------------ */
/* MP4 demuxer                                                         */
/* ------------------------------------------------------------------ */

#define MP4_MAX_ENTRIES 16384

typedef struct {
    FILE *f;
    vidsrc_t *vs;
    char handler[5];
    char fourcc[5];
    uint32_t trak_ts_pending;
    uint32_t stsz_def, stsz_n;
    uint32_t *stsz;
    uint32_t stco_n;
    uint32_t *stco;
    uint32_t stsc_n;
    uint32_t (*stsc)[3];
    uint32_t stts_n;
    uint32_t (*stts)[2];
} mp4ctx_t;

static void mp4_leaf(mp4ctx_t *cx, const uint8_t *tag, long body, long bend)
{
    FILE *f = cx->f;
    uint8_t tmp[16];

    if (!memcmp(tag, "hdlr", 4) && bend - body >= 12) {
        if (fseek(f, body + 8, SEEK_SET) == 0 && fread(tmp, 1, 4, f) == 4)
            memcpy(cx->handler, tmp, 4);
        return;
    }
    if (!memcmp(tag, "mvhd", 4) && bend - body >= 20) {
        if (fseek(f, body, SEEK_SET) == 0 && fread(tmp, 1, 20, f) == 20) {
            cx->vs->dur_ts = rd32be(tmp + 12);
            cx->vs->dur_dur = rd32be(tmp + 16);
        }
        return;
    }
    if (!memcmp(tag, "mdhd", 4) && bend - body >= 20) {
        if (fseek(f, body, SEEK_SET) == 0 && fread(tmp, 1, 20, f) == 20)
            cx->trak_ts_pending = rd32be(tmp + 12);
        return;
    }
    if (!memcmp(tag, "stsd", 4) && bend - body >= 16) {
        if (fseek(f, body + 4, SEEK_SET) == 0 && fread(tmp, 1, 12, f) == 12)
            memcpy(cx->fourcc, tmp + 8, 4);
        return;
    }
    if (!memcmp(tag, "stsz", 4) && bend - body >= 12) {
        if (fseek(f, body, SEEK_SET) != 0 || fread(tmp, 1, 12, f) != 12)
            return;
        uint32_t def = rd32be(tmp + 4);
        uint32_t n = rd32be(tmp + 8);
        if (def) {
            cx->stsz_def = def;
            cx->stsz_n = n;
            return;
        }
        if (!n || n > MP4_MAX_ENTRIES) return;
        uint32_t *tb = malloc((size_t)n * 4);
        if (!tb) return;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t one[4];
            if (fread(one, 1, 4, f) != 4) { free(tb); return; }
            tb[i] = rd32be(one);
        }
        free(cx->stsz);
        cx->stsz = tb;
        cx->stsz_def = 0;
        cx->stsz_n = n;
        return;
    }
    if ((!memcmp(tag, "stco", 4) || !memcmp(tag, "co64", 4)) &&
        bend - body >= 8) {
        if (fseek(f, body, SEEK_SET) != 0 || fread(tmp, 1, 8, f) != 8)
            return;
        uint32_t n = rd32be(tmp + 4);
        if (!n || n > MP4_MAX_ENTRIES) return;
        uint32_t *tb = malloc((size_t)n * 4);
        if (!tb) return;
        bool wide = !memcmp(tag, "co64", 4);
        for (uint32_t i = 0; i < n; i++) {
            uint8_t one[8];
            size_t rb = wide ? 8 : 4;
            if (fread(one, 1, rb, f) != rb) { free(tb); return; }
            tb[i] = wide ? rd32be(one + 4) : rd32be(one);
        }
        free(cx->stco);
        cx->stco = tb;
        cx->stco_n = n;
        return;
    }
    if (!memcmp(tag, "stsc", 4) && bend - body >= 8) {
        if (fseek(f, body, SEEK_SET) != 0 || fread(tmp, 1, 8, f) != 8)
            return;
        uint32_t n = rd32be(tmp + 4);
        if (!n || n > MP4_MAX_ENTRIES) return;
        uint32_t (*tb)[3] = malloc((size_t)n * 12);
        if (!tb) return;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t one[12];
            if (fread(one, 1, 12, f) != 12) { free(tb); return; }
            tb[i][0] = rd32be(one);
            tb[i][1] = rd32be(one + 4);
            tb[i][2] = rd32be(one + 8);
        }
        free(cx->stsc);
        cx->stsc = tb;
        cx->stsc_n = n;
        return;
    }
    if (!memcmp(tag, "stts", 4) && bend - body >= 8) {
        if (fseek(f, body, SEEK_SET) != 0 || fread(tmp, 1, 8, f) != 8)
            return;
        uint32_t n = rd32be(tmp + 4);
        if (!n || n > MP4_MAX_ENTRIES) return;
        uint32_t (*tb)[2] = malloc((size_t)n * 8);
        if (!tb) return;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t one[8];
            if (fread(one, 1, 8, f) != 8) { free(tb); return; }
            tb[i][0] = rd32be(one);
            tb[i][1] = rd32be(one + 4);
        }
        free(cx->stts);
        cx->stts = tb;
        cx->stts_n = n;
        return;
    }
}

static bool mp4_boxes(mp4ctx_t *cx, long start, long end, int depth)
{
    static const char *const cont[] = { "moov", "mdia", "minf", "stbl",
                                        NULL };
    FILE *f = cx->f;
    long off = start;
    uint8_t h[8];

    while (off + 8 <= end) {
        if (fseek(f, off, SEEK_SET) != 0 || fread(h, 1, 8, f) != 8)
            return false;
        uint32_t sz32 = rd32be(h);
        long body = off + 8;
        long sz;
        if (sz32 == 1) {
            uint8_t b8[8];
            if (fread(b8, 1, 8, f) != 8) return false;
            uint64_t big = ((uint64_t)rd32be(b8) << 32) | rd32be(b8 + 4);
            if (big > 0x70000000ull) return false;
            sz = (long)big;
        } else if (sz32 == 0) {
            sz = end - off;
        } else {
            sz = (long)sz32;
        }
        if (body + sz > end) sz = end - body;
        if (sz <= 0) return false;

        if (!memcmp(h + 4, "trak", 4)) {
            char hsav[5], fsav[5];
            memcpy(hsav, cx->handler, 5);
            memcpy(fsav, cx->fourcc, 5);
            memset(cx->handler, 0, 5);
            memset(cx->fourcc, 0, 5);
            uint32_t tsav = cx->trak_ts_pending;
            cx->trak_ts_pending = 0;
            uint32_t sd = cx->stsz_def, sn = cx->stsz_n;
            uint32_t *sp = cx->stsz;
            uint32_t cn = cx->stco_n;
            uint32_t *cp = cx->stco;
            uint32_t qn = cx->stsc_n;
            uint32_t (*qp)[3] = cx->stsc;
            uint32_t tn = cx->stts_n;
            uint32_t (*tp)[2] = cx->stts;
            cx->stsz_def = 0;
            cx->stsz_n = 0;
            cx->stsz = NULL;
            cx->stco_n = 0;
            cx->stco = NULL;
            cx->stsc_n = 0;
            cx->stsc = NULL;
            cx->stts_n = 0;
            cx->stts = NULL;

            mp4_boxes(cx, body, body + sz, depth + 1);

            bool jpeg_codec =
                !memcmp(cx->fourcc, "jpeg", 4) ||
                !memcmp(cx->fourcc, "JPEG", 4);
            bool keep = !memcmp(cx->handler, "vide", 4) && jpeg_codec &&
                        cx->stco_n > 0 && cx->stsc_n > 0 &&
                        cx->stsz_n > 0 &&
                        (cx->stsz_def || cx->stsz);

            if (keep && cx->vs->stco_n == 0) {
                vidsrc_t *v = cx->vs;
                v->stsz_def = cx->stsz_def;
                v->stsz_n = cx->stsz_n;
                v->stsz = cx->stsz;
                v->stco_n = cx->stco_n;
                v->stco = cx->stco;
                v->stsc_n = cx->stsc_n;
                v->stsc = cx->stsc;
                memcpy(v->codec, cx->fourcc, 4);

                /* average sample delta -> fps */
                if (cx->stts_n && cx->trak_ts_pending) {
                    uint64_t wsum = 0, dsum = 0;
                    for (uint32_t i = 0; i < cx->stts_n; i++) {
                        wsum += cx->stts[i][0];
                        dsum += (uint64_t)cx->stts[i][0] * cx->stts[i][1];
                    }
                    if (wsum && dsum) {
                        uint64_t avg_delta = dsum / wsum;
                        uint64_t f1000 =
                            (uint64_t)cx->trak_ts_pending * 1000ull /
                            avg_delta;
                        if (f1000 < 100) f1000 = 100;
                        if (f1000 > 60000) f1000 = 60000;
                        v->fps_x1000 = (uint32_t)f1000;
                        v->dur_ts = cx->trak_ts_pending;
                    }
                }
            } else {
                if (!memcmp(cx->handler, "vide", 4) &&
                    !cx->vs->codec[0] && cx->fourcc[0])
                    memcpy(cx->vs->codec, cx->fourcc, 4);
                free(cx->stsz);
                free(cx->stco);
                free(cx->stsc);
                free(cx->stts);
            }

            /* restore outer context (nested traks are not expected) */
            memcpy(cx->handler, hsav, 5);
            memcpy(cx->fourcc, fsav, 5);
            cx->trak_ts_pending = tsav;
            cx->stsz_def = sd;
            cx->stsz_n = sn;
            cx->stsz = sp;
            cx->stco_n = cn;
            cx->stco = cp;
            cx->stsc_n = qn;
            cx->stsc = qp;
            cx->stts_n = tn;
            cx->stts = tp;

            off = body + sz;
            continue;
        }

        bool container = false;
        for (int i = 0; cont[i]; i++)
            if (!memcmp(h + 4, cont[i], 4)) container = true;

        if (container && depth < 6) {
            mp4_boxes(cx, body, body + sz, depth + 1);
        } else {
            mp4_leaf(cx, h + 4, body, body + sz);
        }
        off = body + sz;
    }
    return true;
}

static bool vid_mp4_scan(vidsrc_t *vs)
{
    mp4ctx_t cx;
    memset(&cx, 0, sizeof(cx));
    cx.f = vs->f;
    cx.vs = vs;
    mp4_boxes(&cx, 0, vs->fsize, 0);
    return vs->stco_n > 0 && vs->stsc_n > 0 && vs->stsz_n > 0 &&
           (!memcmp(vs->codec, "jpeg", 4) ||
            !memcmp(vs->codec, "JPEG", 4));
}

/* locate sample idx: returns length and fills data_off, or 0 */
static long vid_mp4_sample(vidsrc_t *vs, uint32_t idx)
{
    if (!vs->stsc_n || !vs->stco_n || !vs->stsz_n || idx >= vs->stsz_n)
        return 0;

    uint32_t acc = 0;
    uint32_t chunk = 0, base = 0;

    for (uint32_t e = 0; e < vs->stsc_n; e++) {
        uint32_t fc = vs->stsc[e][0];
        uint32_t spc = vs->stsc[e][1] ? vs->stsc[e][1] : 1;
        uint32_t next_fc =
            e + 1 < vs->stsc_n ? vs->stsc[e + 1][0] : 0;
        uint32_t run_chunks =
            next_fc ? next_fc - fc
                    : (vs->stco_n >= fc ? vs->stco_n - fc + 1 : 0);
        uint32_t run_samples = run_chunks * spc;
        if (next_fc == 0 || idx < acc + run_samples) {
            if (idx >= acc + run_samples) return 0;
            chunk = fc + (idx - acc) / spc;
            base = acc + ((idx - acc) / spc) * spc;
            break;
        }
        acc += run_samples;
    }
    if (!chunk || chunk > vs->stco_n || base > idx) return 0;

    long off = (long)vs->stco[chunk - 1];
    for (uint32_t s = base; s < idx && s < vs->stsz_n; s++)
        off += vs->stsz_def ? (long)vs->stsz_def : (long)vs->stsz[s];
    long len = vs->stsz_def ? (long)vs->stsz_def : (long)vs->stsz[idx];
    if (len <= 0 || len > 3u * 1024 * 1024) return 0;
    vs->data_off = off;
    return len;
}

/* ------------------------------------------------------------------ */
/* source open/close/dispatch                                          */
/* ------------------------------------------------------------------ */

static void vidsrc_close(vidsrc_t *vs)
{
    if (vs->f) fclose(vs->f);
    free(vs->stsz);
    free(vs->stco);
    free(vs->stsc);
    free(vs->stts);
    free(vs->jpg);
    free(vs->raw);
    memset(vs, 0, sizeof(*vs));
}

/* opens the file; keeps vs->codec filled even on unsupported codecs */
static bool vidsrc_open(vidsrc_t *vs, const char *path)
{
    struct stat st;

    memset(vs, 0, sizeof(*vs));
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
        return false;
    vs->fsize = (long)st.st_size;
    vs->f = fopen(path, "rb");
    if (!vs->f) return false;

    const char *e = med_ext(path);
    bool ok;
    if (!strcasecmp(e, "avi")) {
        vs->kind = VS_AVI;
        ok = vid_avi_scan(vs);
    } else if (!strcasecmp(e, "mp4") || !strcasecmp(e, "m4v") ||
               !strcasecmp(e, "mov")) {
        vs->kind = VS_MP4;
        ok = vid_mp4_scan(vs);
    } else {
        vs->kind = VS_RAW;
        ok = false;
        /* probe: any leading JPEG stream works */
        if (vid_raw_next(vs) > 0) {
            vs->pos = vs->frame_start;
            ok = true;
        }
    }
    if (!ok) {
        fclose(vs->f);
        vs->f = NULL;
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* players                                                             */
/* ------------------------------------------------------------------ */

#define VID_MAX_W 452
#define VID_MAX_H 186
#define VIEW_W    440
#define VIEW_H    196

typedef struct {
    char path[MED_PATH_MAX];
    vidsrc_t vs;
    uint16_t *buf[2];
    int bw[2], bh[2];
    int show_idx;
    volatile bool painted;
    int64_t next_due;
    uint32_t frame_us;
    med_cvt_t cv;
    bool cv_on;
    uint8_t *ablk;               /* compressed audio chunk in */
    int16_t *osc;                /* resampled PCM out */
} vidjob_t;

static int vid_progress_pct(const vidsrc_t *vs)
{
    if (vs->kind == VS_RAW && vs->fsize > 0)
        return (int)((int64_t)vs->pos * 100 / vs->fsize);
    if (vs->kind == VS_AVI && vs->movi_end > vs->movi_start)
        return (int)(((int64_t)(vs->pos - vs->movi_start) * 100) /
                     (vs->movi_end - vs->movi_start));
    if (vs->kind == VS_MP4 && vs->stsz_n)
        return (int)((uint64_t)vs->samp * 100 / vs->stsz_n);
    return 0;
}

static void vid_apply_seek(vidjob_t *job, int pct)
{
    vidsrc_t *vs = &job->vs;

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (vs->kind == VS_RAW) {
        vs->pos = (long)(((uint64_t)vs->fsize * (uint32_t)pct) / 100u);
    } else if (vs->kind == VS_AVI) {
        vid_avi_seek_pct(vs, pct);
    } else if (vs->kind == VS_MP4) {
        vs->samp = (uint32_t)((uint64_t)vs->stsz_n * (uint32_t)pct / 100u);
    }
    if (job->cv_on)
        med_cvt_init(&job->cv, vs->aud_rate ? vs->aud_rate : 16000,
                     vs->aud_ch ? vs->aud_ch : 1,
                     vs->aud_bits ? vs->aud_bits : 16);
    job->next_due = esp_timer_get_time() + (int64_t)job->frame_us;
}

static void med_video_task(void *arg)
{
    med_t *m = (med_t *)arg;
    bool natural_end = false;

    vidjob_t *job = calloc(1, sizeof(*job));
    if (!job) {
        m->vid_busy = false;
        med_touch(m);
        med_worker_exit(m);
    }
    job->painted = true;
    m->vid_job = job;
    snprintf(job->path, sizeof(job->path), "%.170s", m->open_path);

    if (!vidsrc_open(&job->vs, job->path)) {
        if (job->vs.codec[0])
            snprintf(m->note, sizeof(m->note), "codec %.4s unsupported",
                     job->vs.codec);
        else
            snprintf(m->note, sizeof(m->note), "cannot open video");
        goto out;
    }

    job->frame_us = 1000000u / (job->vs.fps_x1000 ? job->vs.fps_x1000
                                                  : 10000u);
    if (job->vs.aud_pcm && job->vs.aud_rate >= 4000 &&
        job->vs.aud_rate <= 96000 &&
        (job->vs.aud_bits == 8 || job->vs.aud_bits == 16) &&
        job->vs.aud_ch >= 1 && job->vs.aud_ch <= 2) {
        med_cvt_init(&job->cv, job->vs.aud_rate, job->vs.aud_ch,
                     job->vs.aud_bits);
        job->cv_on = true;
        job->ablk = malloc(65536);
        job->osc = malloc(4096 * 2 * sizeof(int16_t));
        if (!job->ablk || !job->osc) job->cv_on = false;
    }
    job->next_due = esp_timer_get_time();
    m->vid_pct = 0;

    for (;;) {
        if (m->shutdown || m->vid_stop) break;

        if (m->vid_pause) {
            vTaskDelay(pdMS_TO_TICKS(50));
            job->next_due = esp_timer_get_time() + (int64_t)job->frame_us;
            continue;
        }
        if (m->vid_seek_pct >= 0) {
            vid_apply_seek(job, m->vid_seek_pct);
            m->vid_seek_pct = -1;
        }

        long n;
        bool isaud = false;
        switch (job->vs.kind) {
        case VS_RAW:
            n = vid_raw_next(&job->vs);
            break;
        case VS_AVI:
            n = vid_avi_next(&job->vs, &isaud);
            break;
        default:
            n = vid_mp4_sample(&job->vs, job->vs.samp++);
            break;
        }
        if (n == 0) {
            natural_end = true;
            break;
        }

        if (isaud) {
            long alen = -n;
            if (job->cv_on && job->ablk && job->osc && alen <= 65536 &&
                fseek(job->vs.f, job->vs.data_off, SEEK_SET) == 0 &&
                fread(job->ablk, 1, (size_t)alen, job->vs.f) ==
                    (size_t)alen) {
                med_cvt_feed(&job->cv, job->ablk, (size_t)alen,
                             job->osc, 4096);
            }
            continue;
        }

        int waited = 0;
        while (!job->painted && waited < 150 && !m->shutdown &&
               !m->vid_stop) {
            vTaskDelay(pdMS_TO_TICKS(1));
            waited++;
        }

        int dw = 0, dh = 0;
        uint16_t *pix = arpile_jpeg_decode_hw(job->vs.jpg, (size_t)n,
                                              VID_MAX_W, VID_MAX_H,
                                              800, 800, &dw, &dh);
        if (!pix || !dw || !dh) {
            free(pix);
            continue;
        }

        int bi = job->show_idx ^ 1;
        if (job->bw[bi] != dw || job->bh[bi] != dh) {
            uint16_t *nb =
                realloc(job->buf[bi], (size_t)dw * (size_t)dh * 2);
            if (!nb) {
                free(pix);
                continue;
            }
            job->buf[bi] = nb;
            job->bw[bi] = dw;
            job->bh[bi] = dh;
        }
        memcpy(job->buf[bi], pix, (size_t)dw * (size_t)dh * 2);
        free(pix);

        int64_t now = esp_timer_get_time();
        if (now < job->next_due)
            vTaskDelay(pdMS_TO_TICKS((job->next_due - now) / 1000));
        else if (now > job->next_due + (int64_t)job->frame_us * 3)
            job->next_due = now;
        job->next_due += (int64_t)job->frame_us;

        portENTER_CRITICAL(&g_med_mux);
        m->vid_fbptr = job->buf[bi];
        job->show_idx = bi;
        m->vid_dw = dw;
        m->vid_dh = dh;
        m->vid_seq++;
        portEXIT_CRITICAL(&g_med_mux);
        job->painted = false;
        m->vid_frame++;
        m->vid_pct = vid_progress_pct(&job->vs);
        med_touch(m);
    }

out:
    vidsrc_close(&job->vs);
    free(job->ablk);
    free(job->osc);
    free(job->buf[0]);
    free(job->buf[1]);
    m->vid_fbptr = NULL;
    m->vid_seq++;
    m->vid_busy = false;
    if (m->vid_job == job) m->vid_job = NULL;
    free(job);
    if (natural_end && !m->shutdown && m->mode == MD_VID)
        m->mode = MD_LIB;
    med_touch(m);
    med_worker_exit(m);
}

static void med_viewer_task(void *arg)
{
    med_t *m = (med_t *)arg;
    char path[MED_PATH_MAX];

    snprintf(path, sizeof(path), "%.170s", m->open_path);

    free(m->vpix);
    m->vpix = NULL;
    m->vw = 0;
    m->vh = 0;
    m->vstate = 1;
    med_touch(m);

    uint16_t *pix = NULL;
    int pw = 0, ph = 0;
    const char *e = med_ext(path);
    if (!strcasecmp(e, "bmp")) {
        int w = 0, h = 0;
        uint16_t *full = med_bmp_decode(path, &w, &h);
        if (full) {
            pix = med_scale_fit(full, w, h, VIEW_W, VIEW_H, &pw, &ph);
            free(full);
        }
    } else {
        uint8_t *buf = NULL;
        size_t len = 0;
        if (med_read_file(path, &buf, &len, 6u * 1024 * 1024)) {
            if (len > 4 && buf[0] == 0xFF && buf[1] == 0xD8)
                pix = arpile_jpeg_decode_hw(buf, len, VIEW_W, VIEW_H,
                                            480, 320, &pw, &ph);
            free(buf);
        }
    }

    if (!m->shutdown) {
        m->vpix = pix;
        m->vw = pw;
        m->vh = ph;
        m->vstate = pix ? 2 : 3;
        med_touch(m);
    } else {
        free(pix);
    }
    med_worker_exit(m);
}

static void med_audio_task(void *arg)
{
    med_t *m = (med_t *)arg;
    char path[MED_PATH_MAX];
    int16_t *osc = malloc(4096 * 2 * sizeof(int16_t));
    FILE *f = NULL;
    uint16_t ch = 0, bits = 0;
    uint32_t rate = 0;
    long data_off = 0, data_len = 0;
    struct stat st;

    snprintf(path, sizeof(path), "%.170s", m->open_path);
    if (stat(path, &st) != 0) st.st_size = 0;

    if (osc && st.st_size > 44) {
        f = fopen(path, "rb");
        uint8_t h[12];
        if (f && fread(h, 1, 12, f) == 12 && !memcmp(h, "RIFF", 4) &&
            !memcmp(h + 8, "WAVE", 4)) {
            long off = 12;
            while (off + 8 <= (long)st.st_size) {
                uint8_t chdr[8];
                if (fseek(f, off, SEEK_SET) != 0 ||
                    fread(chdr, 1, 8, f) != 8) break;
                uint32_t csz = rd32le(chdr + 4);
                long cbody = off + 8;
                if (!memcmp(chdr, "fmt ", 4) && csz >= 16) {
                    uint8_t fb_[16];
                    if (fseek(f, cbody, SEEK_SET) == 0 &&
                        fread(fb_, 1, 16, f) == 16) {
                        uint16_t fmt = rd16le(fb_);
                        ch = rd16le(fb_ + 2);
                        rate = rd32le(fb_ + 4);
                        bits = rd16le(fb_ + 14);
                        if (fmt != 1) ch = 0;      /* PCM only */
                    }
                } else if (!memcmp(chdr, "data", 4)) {
                    data_off = cbody;
                    data_len = (long)csz;
                    if (cbody + data_len > (long)st.st_size)
                        data_len = (long)st.st_size - cbody;
                }
                off = cbody + (long)((csz + 1) & ~1u);
            }
        }
        if (f) fclose(f);
        f = NULL;
    }

    if (!osc || !data_len || !ch || (bits != 8 && bits != 16) ||
        rate < 4000 || rate > 192000) {
        snprintf(m->note, sizeof(m->note), "unsupported audio: %.7s",
                 med_ext(path));
        m->aud_busy = false;
        free(osc);
        med_touch(m);
        med_worker_exit(m);
    }

    med_cvt_t cv;
    med_cvt_init(&cv, rate, ch, bits);
    f = fopen(path, "rb");
    uint8_t *blk = f ? malloc(8192) : NULL;
    long done = 0;
    if (f) fseek(f, data_off, SEEK_SET);
    while (!m->shutdown && !m->aud_stop && blk && osc && f) {
        size_t got = fread(blk, 1, 8192, f);
        if (!got) break;
        med_cvt_feed(&cv, blk, got, osc, 4096);
        done += (long)got;
        m->aud_pct = (int)(done * 100 / data_len);
    }
    free(blk);
    if (f) fclose(f);
    free(osc);
    if (!m->shutdown && !m->aud_stop) m->aud_pct = 100;
    m->aud_busy = false;
    med_touch(m);
    med_worker_exit(m);
}

/* ------------------------------------------------------------------ */
/* library UI                                                          */
/* ------------------------------------------------------------------ */

#define LIB_TAB_W    58
#define LIB_TAB_H    22
#define LIB_ROW_H    26
#define LIB_LIST_W   290
#define LIB_FOOT_H   16
#define LIB_QSTRIP_H 20

static const char *const k_cats[4] = { "All", "Video", "Image", "Audio" };
static const char *const k_sorts[3] = { "A-Z", "New", "Big" };

static const char *med_base(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void med_draw_tail(ili9488_t *lcd, int x, int y, int maxw,
                          const char *s, uint16_t fg, uint16_t bg)
{
    while (*s && (int)ui_text_width(s) > maxw) s++;
    ui_draw_text(lcd, (uint16_t)x, (uint16_t)y, s, fg, bg);
}

typedef struct {
    ui_rect_t tabs[4];
    ui_rect_t find, sort;
    ui_rect_t qstrip;
    bool qactive;
    ui_rect_t list, scroll;
    ui_rect_t prev;
    ui_rect_t foot;
} med_layout_t;

static void med_layout(const ui_rect_t *cr, const med_t *m, med_layout_t *L)
{
    int y = cr->y + 2;
    for (int i = 0; i < 4; i++)
        L->tabs[i] = ui_rect((uint16_t)(cr->x + 2 + i * (LIB_TAB_W + 2)),
                             (uint16_t)y, LIB_TAB_W, LIB_TAB_H);
    L->find = ui_rect((uint16_t)(cr->x + 2 + 4 * (LIB_TAB_W + 2)),
                      (uint16_t)y, 42, LIB_TAB_H);
    L->sort = ui_rect((uint16_t)(L->find.x + L->find.w + 2),
                      (uint16_t)y, 46, LIB_TAB_H);

    y += LIB_TAB_H + 3;
    L->qactive = m->query_edit || m->query[0];
    if (L->qactive) {
        L->qstrip = ui_rect((uint16_t)(cr->x + 2), (uint16_t)y,
                            (uint16_t)(cr->w - 4), LIB_QSTRIP_H);
        y += LIB_QSTRIP_H + 2;
    }

    int foot_y = cr->y + (int)cr->h - LIB_FOOT_H;
    L->list = ui_rect((uint16_t)(cr->x + 2), (uint16_t)y, LIB_LIST_W,
                      (uint16_t)(foot_y - y));
    L->scroll = ui_rect((uint16_t)(L->list.x + LIB_LIST_W + 1),
                        (uint16_t)y, 6, (uint16_t)(foot_y - y));
    L->prev = ui_rect((uint16_t)(cr->x + 304), (uint16_t)y,
                      (uint16_t)(cr->w - 306),
                      (uint16_t)(foot_y - y));
    L->foot = ui_rect(cr->x, (uint16_t)foot_y, cr->w, LIB_FOOT_H);
}

static void med_button(ili9488_t *lcd, const ui_rect_t *r, const char *label,
                       bool active)
{
    ui_draw_fill_rect(lcd, r, active ? UI_C_BUTTON_DOWN : UI_C_BUTTON);
    ui_draw_outline(lcd, r, active ? UI_C_ACCENT : UI_C_BORDER);
    ui_draw_text(lcd,
                 (uint16_t)(r->x + ((int)r->w - (int)ui_text_width(label)) / 2),
                 (uint16_t)(r->y + ((int)r->h - CHAR_H) / 2),
                 label, UI_C_TEXT, UI_C_WIN_BG);
}

static void med_render_library(med_t *m, ili9488_t *lcd, const ui_rect_t *cr)
{
    med_layout_t L;
    med_layout(cr, m, &L);

    ui_draw_fill_rect(lcd, cr, UI_C_WIN_BG);

    for (int i = 0; i < 4; i++)
        med_button(lcd, &L.tabs[i], k_cats[i],
                   (i == 0 && m->cat < 0) || m->cat == i - 1);
    med_button(lcd, &L.find, "Find", m->query_edit);
    med_button(lcd, &L.sort, k_sorts[m->sort % 3], false);

    if (L.qactive) {
        char q[64];
        snprintf(q, sizeof(q), "find: %s%s", m->query,
                 m->query_edit ? "_" : "");
        ui_draw_fill_rect(lcd, &L.qstrip, UI_C_PANEL);
        ui_draw_text(lcd, (uint16_t)(L.qstrip.x + 6),
                     (uint16_t)(L.qstrip.y + 3), q, UI_C_ACCENT,
                     UI_C_PANEL);
    }

    int total = med_count_visible(m);
    int vis = L.list.h / LIB_ROW_H;
    if (vis < 1) vis = 1;
    if (m->top > total - vis) m->top = total - vis;
    if (m->top < 0) m->top = 0;

    for (int row = 0; row < vis; row++) {
        int pos = m->top + row;
        if (pos >= total) break;
        int idx = med_pos_to_idx(m, pos);
        if (idx < 0) continue;
        const med_item_t *it = &m->items[idx];
        ui_rect_t rr = ui_rect(L.list.x, (uint16_t)(L.list.y + row * LIB_ROW_H),
                               LIB_LIST_W, LIB_ROW_H);
        bool sel = pos == m->cursor;
        if (sel) {
            ui_rect_t hb = ui_rect(rr.x, rr.y, (uint16_t)(rr.w - 2), rr.h);
            ui_draw_fill_rect(lcd, &hb, UI_C_PANEL_HI);
            ui_draw_vline(lcd, rr.x, rr.y, rr.h, UI_C_ACCENT);
        }
        ui_draw_icon(lcd, med_type_icon(it->type),
                     (uint16_t)(rr.x + 5), (uint16_t)(rr.y + 5),
                     sel ? UI_C_TEXT : UI_C_TEXT_DIM, UI_C_WIN_BG);
        char sz[12];
        med_human_size(it->size, sz, sizeof(sz));
        int sw = (int)ui_text_width(sz);
        med_draw_tail(lcd, rr.x + 26, rr.y + 6,
                      LIB_LIST_W - 40 - sw, med_base(it->path),
                      sel ? UI_C_TEXT : UI_C_TEXT_DIM, UI_C_WIN_BG);
        ui_draw_text(lcd, (uint16_t)(rr.x + rr.w - 8 - sw),
                     (uint16_t)(rr.y + 6), sz, UI_C_TEXT_DIM, UI_C_WIN_BG);
    }

    if (total > vis) {
        ui_draw_fill_rect(lcd, &L.scroll, UI_C_PANEL);
        int thumb_h = vis * L.scroll.h / total;
        if (thumb_h < 10) thumb_h = 10;
        int max_off = L.scroll.h - thumb_h;
        int off = total > vis ? (max_off * m->top) / (total - vis) : 0;
        ui_rect_t th = ui_rect(L.scroll.x,
                               (uint16_t)(L.scroll.y + off),
                               L.scroll.w, (uint16_t)thumb_h);
        ui_draw_fill_rect(lcd, &th, UI_C_ACCENT);
    }

    ui_draw_vline(lcd, L.prev.x - 3, L.prev.y, L.prev.h, UI_C_BORDER);

    /* preview pane */
    ui_rect_t box = ui_rect((uint16_t)(L.prev.x + 4), L.prev.y,
                            (uint16_t)(L.prev.w - 8), 120);
    int idx = m->prev_idx >= 0 && m->prev_idx < m->n_items
                  ? m->prev_idx : -1;
    if (idx >= 0 && m->prev_state == 2 && m->prev_pix) {
        int dx = box.x + ((int)box.w - m->prev_w) / 2;
        int dy = box.y + ((int)box.h - m->prev_h) / 2;
        for (int yy = 0; yy < m->prev_h; yy++)
            ili9488_draw_pixels(lcd, m->prev_pix, (uint16_t)dx,
                                (uint16_t)(dy + yy), (uint16_t)m->prev_w,
                                1, 0, (uint16_t)yy, (uint16_t)m->prev_w,
                                1, (uint16_t)m->prev_w);
    } else {
        const char *ic = idx >= 0 ? med_type_icon(m->items[idx].type)
                                  : "movie";
        ui_draw_icon_scaled(lcd, ic,
                            (uint16_t)(box.x + (int)box.w / 2 - 24),
                            (uint16_t)(box.y + 30), 3,
                            UI_C_BORDER, UI_C_WIN_BG);
        if (idx >= 0 && m->items[idx].type == MED_T_IMAGE &&
            m->prev_state == 1)
            ui_draw_text(lcd, (uint16_t)(box.x + 34),
                         (uint16_t)(box.y + 92), "loading...",
                         UI_C_TEXT_DIM, UI_C_WIN_BG);
    }

    if (idx >= 0) {
        const med_item_t *it = &m->items[idx];
        char info[48];
        med_draw_tail(lcd, L.prev.x + 4, box.y + box.h + 6,
                      L.prev.w - 8, med_base(it->path),
                      UI_C_TEXT, UI_C_WIN_BG);
        med_human_size(it->size, info, sizeof(info));
        {
            size_t l = strlen(info);
            snprintf(info + l, sizeof(info) - l, "  %s",
                     it->type == MED_T_VIDEO ? "video" :
                     it->type == MED_T_AUDIO ? "audio" : "image");
        }
        ui_draw_text(lcd, (uint16_t)(L.prev.x + 4),
                     (uint16_t)(box.y + box.h + 24), info,
                     UI_C_TEXT_DIM, UI_C_WIN_BG);
        if (it->type == MED_T_AUDIO && m->aud_busy &&
            !strcmp(m->aud_name, med_base(it->path)))
            ui_draw_text(lcd, (uint16_t)(L.prev.x + 4),
                         (uint16_t)(box.y + box.h + 40), "playing...",
                         UI_C_ACCENT, UI_C_WIN_BG);
    }

    /* footer */
    char foot[96];
    int per[3] = { 0, 0, 0 };
    for (int i = 0; i < m->n_items; i++)
        if (m->items[i].type <= MED_T_AUDIO) per[m->items[i].type]++;
    if (m->scanning) {
        snprintf(foot, sizeof(foot), "scan %.52s  +%d new ~%d chg -%d gone",
                 m->scan_dir, m->scan_new, m->scan_chg, m->scan_gone);
    } else if (m->nosd) {
        snprintf(foot, sizeof(foot), "%s", "no SD card mounted");
    } else {
        snprintf(foot, sizeof(foot), "%d items  V%d I%d A%d",
                 m->n_items, per[MED_T_VIDEO], per[MED_T_IMAGE],
                 per[MED_T_AUDIO]);
    }
    ui_draw_fill_rect(lcd, &L.foot, UI_C_PANEL);
    med_draw_tail(lcd, L.foot.x + 6, L.foot.y + 1, L.foot.w - 12, foot,
                  UI_C_TEXT_DIM, UI_C_PANEL);
}

/* ------------------------------------------------------------------ */
/* viewer / player rendering                                           */
/* ------------------------------------------------------------------ */

static void med_blit(ili9488_t *lcd, const uint16_t *pix, int iw, int ih,
                     int dx, int dy, const ui_rect_t *clip)
{
    int sx = 0, sy = 0;

    if (dx < clip->x) { sx = clip->x - dx; dx = clip->x; }
    if (dy < clip->y) { sy = clip->y - dy; dy = clip->y; }
    int w = iw - sx, h = ih - sy;
    if (dx + w > clip->x + (int)clip->w) w = clip->x + (int)clip->w - dx;
    if (dy + h > clip->y + (int)clip->h) h = clip->y + (int)clip->h - dy;
    if (w <= 0 || h <= 0 || !pix) return;
    for (int ry = 0; ry < h; ry++)
        ili9488_draw_pixels(lcd, pix + (size_t)(sy + ry) * iw + sx,
                            (uint16_t)dx, (uint16_t)(dy + ry),
                            (uint16_t)w, 1, 0, 0, (uint16_t)w, 1,
                            (uint16_t)w);
}

static void med_render_viewer(med_t *m, ili9488_t *lcd, const ui_rect_t *cr)
{
    ui_rect_t black = *cr;
    ui_draw_fill_rect(lcd, &black, UI_RGB(0, 0, 0));

    if (m->vstate == 2 && m->vpix && m->vw > 0 && m->vh > 0) {
        med_blit(lcd, m->vpix, m->vw, m->vh,
                 cr->x + ((int)cr->w - m->vw) / 2,
                 cr->y + ((int)cr->h - m->vh) / 2, cr);
        int total = med_count_visible(m);
        char tag[32];
        snprintf(tag, sizeof(tag), "[%d/%d]", m->cursor + 1, total);
        ui_draw_text(lcd, (uint16_t)(cr->x + 6), (uint16_t)(cr->y + 4),
                     tag, UI_C_TEXT, UI_RGB(0, 0, 0));
    } else if (m->vstate == 1) {
        ui_draw_text(lcd, (uint16_t)(cr->x + cr->w / 2 - 36),
                     (uint16_t)(cr->y + cr->h / 2), "decoding...",
                     UI_C_TEXT_DIM, UI_RGB(0, 0, 0));
    } else {
        ui_draw_text(lcd, (uint16_t)(cr->x + cr->w / 2 - 52),
                     (uint16_t)(cr->y + cr->h / 2), "cannot decode",
                     UI_C_TEXT_DIM, UI_RGB(0, 0, 0));
    }

    ui_rect_t bar = ui_rect(cr->x, (uint16_t)(cr->y + cr->h - 18),
                            cr->w, 18);
    ui_draw_fill_rect(lcd, &bar, UI_C_PANEL);
    ui_draw_text(lcd, (uint16_t)(bar.x + 6), (uint16_t)(bar.y + 2),
                 "Esc back   <-/-> browse", UI_C_TEXT_DIM, UI_C_PANEL);
}

static void med_progress_bar(ili9488_t *lcd, const ui_rect_t *r, int pct)
{
    ui_draw_outline(lcd, r, UI_C_BORDER);
    int fw = ((int)r->w - 4) * pct / 100;
    if (fw > 0) {
        ui_rect_t f = ui_rect((uint16_t)(r->x + 2),
                              (uint16_t)(r->y + 2), (uint16_t)fw,
                              (uint16_t)(r->h - 4));
        ui_draw_fill_rect(lcd, &f, UI_C_ACCENT);
    }
}

static void med_render_video(med_t *m, ili9488_t *lcd, const ui_rect_t *cr)
{
    ui_draw_fill_rect(lcd, cr, UI_RGB(0, 0, 0));

    uint16_t *fb;
    portENTER_CRITICAL(&g_med_mux);
    fb = m->vid_fbptr;
    portEXIT_CRITICAL(&g_med_mux);

    if (!m->vid_busy && !fb && m->note[0]) {
        ui_draw_text(lcd, (uint16_t)(cr->x + 20),
                     (uint16_t)(cr->y + cr->h / 2 - 7), m->note,
                     UI_C_TEXT_DIM, UI_RGB(0, 0, 0));
    } else if (fb && m->vid_dw > 0 && m->vid_dh > 0) {
        vidjob_t *job = (vidjob_t *)m->vid_job;
        med_blit(lcd, fb, m->vid_dw, m->vid_dh,
                 cr->x + ((int)cr->w - m->vid_dw) / 2,
                 cr->y + ((int)cr->h - 26 - m->vid_dh) / 2, cr);
        if (job) job->painted = true;
    } else {
        ui_draw_text(lcd, (uint16_t)(cr->x + cr->w / 2 - 34),
                     (uint16_t)(cr->y + cr->h / 2), "loading...",
                     UI_C_TEXT_DIM, UI_RGB(0, 0, 0));
    }

    ui_rect_t strip = ui_rect(cr->x, (uint16_t)(cr->y + cr->h - 26),
                              cr->w, 26);
    ui_draw_fill_rect(lcd, &strip, UI_C_PANEL);
    ui_draw_icon(lcd, m->vid_pause ? "play_arrow" : "pause",
                 (uint16_t)(strip.x + 6), (uint16_t)(strip.y + 5),
                 UI_C_TEXT, UI_C_PANEL);
    ui_rect_t pb = ui_rect((uint16_t)(strip.x + 30),
                           (uint16_t)(strip.y + 9),
                           (uint16_t)(strip.w - 130), 8);
    med_progress_bar(lcd, &pb, m->vid_pct);
    char txt[24];
    snprintf(txt, sizeof(txt), "%d%%", m->vid_pct);
    ui_draw_text(lcd, (uint16_t)(pb.x + pb.w + 6),
                 (uint16_t)(strip.y + 6), txt, UI_C_TEXT_DIM, UI_C_PANEL);
    snprintf(txt, sizeof(txt), "vol %d", arpile_audio_get_volume());
    ui_draw_text(lcd, (uint16_t)(strip.x + strip.w - 62),
                 (uint16_t)(strip.y + 6), txt, UI_C_TEXT_DIM, UI_C_PANEL);
}

static void med_render_audio(med_t *m, ili9488_t *lcd, const ui_rect_t *cr)
{
    ui_draw_fill_rect(lcd, cr, UI_C_WIN_BG);

    ui_draw_icon_scaled(lcd, "music_note",
                        (uint16_t)(cr->x + cr->w / 2 - 32),
                        (uint16_t)(cr->y + 22), 4,
                        UI_C_ACCENT, UI_C_WIN_BG);
    med_draw_tail(lcd, cr->x + 16, cr->y + 100, cr->w - 32,
                  m->aud_name, UI_C_TEXT, UI_C_WIN_BG);

    ui_rect_t pb = ui_rect((uint16_t)(cr->x + 30),
                           (uint16_t)(cr->y + 126),
                           (uint16_t)(cr->w - 60), 12);
    med_progress_bar(lcd, &pb, m->aud_pct);
    char txt[40];
    snprintf(txt, sizeof(txt), "%d%%   vol %d", m->aud_pct,
             arpile_audio_get_volume());
    ui_draw_text(lcd, (uint16_t)(cr->x + 30), (uint16_t)(pb.y + 22),
                 txt, UI_C_TEXT_DIM, UI_C_WIN_BG);
    ui_draw_text(lcd, (uint16_t)(cr->x + 16), (uint16_t)(cr->y + cr->h - 20),
                 "Space stop   Esc back", UI_C_TEXT_DIM, UI_C_WIN_BG);
}

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */

static void med_open_item(med_t *m, int idx)
{
    if (idx < 0 || idx >= m->n_items) return;
    const med_item_t *it = &m->items[idx];

    snprintf(m->open_path, sizeof(m->open_path), "%.170s", it->path);

    if (it->type == MED_T_IMAGE) {
        if (m->vstate == 1) return;
        m->mode = MD_IMG;
        m->vstate = 0;
        med_spawn(m, med_viewer_task, "med_view", 6144, 5);
    } else if (it->type == MED_T_VIDEO) {
        if (m->vid_busy) return;
        m->vid_stop = false;
        m->vid_pause = false;
        m->vid_seek_pct = -1;
        m->vid_pct = 0;
        m->vid_fbptr = NULL;
        m->note[0] = '\0';
        m->vid_busy = true;
        m->mode = MD_VID;
        med_spawn(m, med_video_task, "med_vid", 8192, 5);
    } else if (it->type == MED_T_AUDIO) {
        if (m->aud_busy) return;
        if (!arpile_audio_ready()) {
            snprintf(m->note, sizeof(m->note), "audio codec not ready");
            m->note[0] = '\0';
            return;
        }
        m->aud_stop = false;
        m->aud_pct = 0;
        snprintf(m->aud_name, sizeof(m->aud_name), "%.34s",
                 med_base(it->path));
        m->mode = MD_AUD;
        m->aud_busy = true;
        med_spawn(m, med_audio_task, "med_aud", 4096, 5);
    }
    med_touch(m);
}

static void med_leave_video(med_t *m)
{
    if (m->vid_busy) m->vid_stop = true;
    m->mode = MD_LIB;
    med_touch(m);
}

static void med_adjust_sel(med_t *m, int delta, int vis)
{
    int total = med_count_visible(m);

    m->cursor += delta;
    if (m->cursor >= total) m->cursor = total - 1;
    if (m->cursor < 0) m->cursor = 0;
    if (m->top > total - vis) m->top = total - vis;
    if (m->top < 0) m->top = 0;
    while (m->cursor < m->top) m->top--;
    while (m->cursor >= m->top + vis) m->top++;
    med_touch(m);
}

static void med_update_preview(med_t *m)
{
    int sel = m->cursor >= 0 ? med_pos_to_idx(m, m->cursor) : -1;

    if (sel == m->prev_idx) return;
    bool busy = m->prev_state == 1;
    m->prev_idx = sel;
    if (!busy) {
        free(m->prev_pix);
        m->prev_pix = NULL;
        m->prev_w = 0;
        m->prev_h = 0;
        m->prev_state = 0;
        if (sel >= 0 && sel < m->n_items &&
            m->items[sel].type == MED_T_IMAGE)
            med_spawn(m, med_preview_task, "med_prev", 6144, 4);
        med_touch(m);
    }
}

static void med_step_image(med_t *m, int dir)
{
    int total = med_count_visible(m);

    for (int pos = m->cursor + dir; pos >= 0 && pos < total; pos += dir) {
        int idx = med_pos_to_idx(m, pos);
        if (idx >= 0 && m->items[idx].type == MED_T_IMAGE) {
            m->cursor = pos;
            med_open_item(m, idx);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* events                                                              */
/* ------------------------------------------------------------------ */

static void med_event(arpile_app_ctx_t *ctx, const arpile_input_event_t *ev)
{
    med_t *m = (med_t *)ctx->user;

    if (!m || !ctx->win) return;
    ui_rect_t cr = win_client_rect(ctx->win);

    if (ev->type == ARPILE_IN_EVENT_KEY_DOWN) {
        uint16_t k = ev->key.keycode;
        char a = ev->key.ascii;

        switch (m->mode) {
        case MD_LIB:
            if (m->query_edit) {
                if (a >= 32 && a < 127 &&
                    strlen(m->query) < sizeof(m->query) - 1) {
                    size_t l = strlen(m->query);
                    m->query[l++] = a;
                    m->query[l] = '\0';
                } else if (k == ARPILE_KEY_BACKSPACE || a == 8) {
                    size_t l = strlen(m->query);
                    if (l) m->query[l - 1] = '\0';
                } else if (k == ARPILE_KEY_ENTER) {
                    m->query_edit = false;
                } else if (k == ARPILE_KEY_ESCAPE) {
                    m->query[0] = '\0';
                    m->query_edit = false;
                } else {
                    return;
                }
                m->cursor = 0;
                m->top = 0;
                med_touch(m);
                return;
            }

            if (k == ARPILE_KEY_UP) med_adjust_sel(m, -1, cr.h / LIB_ROW_H);
            else if (k == ARPILE_KEY_DOWN)
                med_adjust_sel(m, 1, cr.h / LIB_ROW_H);
            else if (k == ARPILE_KEY_PGUP)
                med_adjust_sel(m, -(cr.h / LIB_ROW_H), cr.h / LIB_ROW_H);
            else if (k == ARPILE_KEY_PGDN)
                med_adjust_sel(m, cr.h / LIB_ROW_H, cr.h / LIB_ROW_H);
            else if (k == ARPILE_KEY_HOME) {
                m->cursor = 0;
                m->top = 0;
                med_touch(m);
            } else if (k == ARPILE_KEY_END)
                med_adjust_sel(m, med_count_visible(m),
                               cr.h / LIB_ROW_H);
            else if (k == ARPILE_KEY_LEFT || k == ARPILE_KEY_RIGHT) {
                int ci = m->cat + 1;
                ci += k == ARPILE_KEY_RIGHT ? 1 : -1;
                if (ci < 0) ci = 0;
                if (ci > 3) ci = 3;
                m->cat = ci - 1;
                m->cursor = 0;
                m->top = 0;
                med_touch(m);
            } else if (k == ARPILE_KEY_ENTER) {
                int idx = med_pos_to_idx(m, m->cursor);
                med_open_item(m, idx);
            } else if (a == 'f') {
                m->query_edit = true;
                med_touch(m);
            } else if (a == 's') {
                m->sort = (m->sort + 1) % 3;
                med_touch(m);
            } else if (k == ARPILE_KEY_ESCAPE && m->query[0]) {
                m->query[0] = '\0';
                m->cursor = 0;
                m->top = 0;
                med_touch(m);
            }
            break;

        case MD_IMG:
            if (k == ARPILE_KEY_ESCAPE || k == ARPILE_KEY_ENTER ||
                k == ARPILE_KEY_SPACE)
                m->mode = MD_LIB;
            else if (k == ARPILE_KEY_LEFT) med_step_image(m, -1);
            else if (k == ARPILE_KEY_RIGHT) med_step_image(m, 1);
            else break;
            med_touch(m);
            break;

        case MD_VID:
            if (k == ARPILE_KEY_ESCAPE) med_leave_video(m);
            else if (k == ARPILE_KEY_SPACE) {
                if (m->vid_busy) m->vid_pause = !m->vid_pause;
            } else if (k == ARPILE_KEY_LEFT || k == ARPILE_KEY_RIGHT) {
                if (m->vid_busy) {
                    int p = m->vid_seek_pct >= 0 ? m->vid_seek_pct
                                                 : m->vid_pct;
                    p += k == ARPILE_KEY_RIGHT ? 5 : -5;
                    if (p < 0) p = 0;
                    if (p > 100) p = 100;
                    m->vid_seek_pct = p;
                }
            } else if (k == ARPILE_KEY_UP || k == ARPILE_KEY_DOWN) {
                int v = arpile_audio_get_volume() +
                        (k == ARPILE_KEY_UP ? 10 : -10);
                arpile_audio_set_volume(v);
            } else break;
            med_touch(m);
            break;

        case MD_AUD:
            if (k == ARPILE_KEY_ESCAPE || k == ARPILE_KEY_ENTER)
                m->mode = MD_LIB;
            else if (k == ARPILE_KEY_SPACE) {
                if (m->aud_busy) m->aud_stop = true;
            } else if (k == ARPILE_KEY_UP || k == ARPILE_KEY_DOWN) {
                int v = arpile_audio_get_volume() +
                        (k == ARPILE_KEY_UP ? 10 : -10);
                arpile_audio_set_volume(v);
            } else break;
            med_touch(m);
            break;
        }
        return;
    }

    if (ev->type == ARPILE_IN_EVENT_MOUSE_WHEEL && m->mode == MD_LIB) {
        med_adjust_sel(m, ev->wheel > 0 ? -3 : 3, cr.h / LIB_ROW_H);
        return;
    }

    if (ev->type == ARPILE_IN_EVENT_MOUSE_BTN &&
        (ev->mouse.buttons & ARPILE_MOUSE_BTN_LEFT)) {
        int mx = ev->mouse.x, my = ev->mouse.y;

        if (m->mode == MD_LIB) {
            med_layout_t L;
            med_layout(&cr, m, &L);

            for (int i = 0; i < 4; i++) {
                if (ui_rect_contains(&L.tabs[i], (int16_t)mx,
                                     (int16_t)my)) {
                    m->cat = i - 1;
                    m->cursor = 0;
                    m->top = 0;
                    med_touch(m);
                    return;
                }
            }
            if (ui_rect_contains(&L.find, (int16_t)mx, (int16_t)my)) {
                m->query_edit = true;
                med_touch(m);
                return;
            }
            if (ui_rect_contains(&L.sort, (int16_t)mx, (int16_t)my)) {
                m->sort = (m->sort + 1) % 3;
                med_touch(m);
                return;
            }
            if (L.qactive &&
                ui_rect_contains(&L.qstrip, (int16_t)mx, (int16_t)my)) {
                m->query_edit = true;
                med_touch(m);
                return;
            }
            if (ui_rect_contains(&L.list, (int16_t)mx, (int16_t)my)) {
                int row = (my - L.list.y) / LIB_ROW_H;
                int pos = m->top + row;
                int total = med_count_visible(m);
                if (pos >= 0 && pos < total) {
                    m->cursor = pos;
                    med_open_item(m, med_pos_to_idx(m, pos));
                }
                return;
            }
            if (ui_rect_contains(&L.scroll, (int16_t)mx, (int16_t)my)) {
                int vis = cr.h / LIB_ROW_H;
                med_adjust_sel(m, my < L.scroll.y + (int)L.scroll.h / 2
                                      ? -vis / 2 : vis / 2, vis);
                return;
            }
        } else if (m->mode == MD_IMG) {
            m->mode = MD_LIB;
            med_touch(m);
        } else if (m->mode == MD_VID) {
            if (my >= cr.y + (int)cr.h - 26) {
                med_layout_t L;
                med_layout(&cr, m, &L);
                ui_rect_t strip = cr;
                (void)strip;
                if (mx >= cr.x + 30 && mx < cr.x + (int)cr.w - 100) {
                    if (m->vid_busy) {
                        int pct = (mx - (cr.x + 30)) * 100 /
                                  ((int)cr.w - 130);
                        if (pct < 0) pct = 0;
                        if (pct > 100) pct = 100;
                        m->vid_seek_pct = pct;
                        med_touch(m);
                    }
                    return;
                }
                if (m->vid_busy && mx < cr.x + 28) {
                    m->vid_pause = !m->vid_pause;
                    med_touch(m);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void med_init(arpile_app_ctx_t *ctx)
{
    med_t *m = calloc(1, sizeof(*m));

    if (!m) return;
    ctx->user = m;
    m->cat = -1;
    m->prev_idx = -1;
    m->vid_seek_pct = -1;

    DIR *d = opendir(MED_SD_ROOT);
    if (!d) {
        m->nosd = true;
        snprintf(m->note, sizeof(m->note), "no SD card");
        med_touch(m);
        return;
    }
    closedir(d);

    mkdir(MED_IDX_DIR, 0775);
    med_load_db(m);
    m->scanning = true;
    med_spawn(m, med_scan_task, "med_scan", 6144, 4);
    med_touch(m);
}

static void med_update(arpile_app_ctx_t *ctx)
{
    med_t *m = (med_t *)ctx->user;

    if (!m) return;
    med_update_preview(m);
    if (!m->dirty) return;
    m->dirty = 0;
    if (ctx->win) arpile_ui_win_redraw(ctx->win);
}

static void med_render(arpile_app_ctx_t *ctx, ui_win_t *win)
{
    med_t *m = (med_t *)ctx->user;

    if (!m) return;
    ui_rect_t cr = win_client_rect(win);
    ili9488_t *lcd = arpile_ui_get_lcd();

    switch (m->mode) {
    case MD_IMG: med_render_viewer(m, lcd, &cr); break;
    case MD_VID: med_render_video(m, lcd, &cr); break;
    case MD_AUD: med_render_audio(m, lcd, &cr); break;
    default:     med_render_library(m, lcd, &cr); break;
    }
}

static void med_destroy(arpile_app_ctx_t *ctx)
{
    med_t *m = (med_t *)ctx->user;

    if (!m) return;
    ctx->user = NULL;

    portENTER_CRITICAL(&g_med_mux);
    m->shutdown = true;
    bool any = m->workers > 0;
    portEXIT_CRITICAL(&g_med_mux);

    if (!any) {
        free(m->items);
        free(m->prev_pix);
        free(m->vpix);
        free(m->vid_job);
        free(m);
    }
}

static const arpile_app_ops_t MED_OPS = {
    .init = med_init,
    .update = med_update,
    .event = med_event,
    .render = med_render,
    .destroy = med_destroy,
};

const arpile_app_t arpile_app_vlc = {
    .id = "vlc",
    .name = "Media",
    .icon = "vlc",
    .ops = &MED_OPS,
};
