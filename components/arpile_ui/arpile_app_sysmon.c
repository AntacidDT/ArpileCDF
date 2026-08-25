/* Sysmon ACEXE — page-based system monitor with live graphs.
 *
 * Pages: [CPU] [RAM] [STORAGE] [GENERAL] [PERIPH], switched with Tab /
 * Shift+Tab / Left / Right or by clicking the tab strip. CPU and RAM pages
 * carry scrolling graphs fed by fixed-size circular histories sampled at
 * 2.5 Hz. Only reports information already exposed by existing subsystems;
 * anything without a status source shows N/A. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/statvfs.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_partition.h"
#include "esp_app_desc.h"

#include "arpile_ui.h"
#include "arpile_app.h"
#include "wifi_test.h"

#define GRAPH_SAMPLES 120            /* ~50 s window at 2.5 Hz */
#define SAMPLE_US     (400 * 1000)
#define MAX_TASKS     40
#define NAME_MAXLEN   configMAX_TASK_NAME_LEN

enum { PAGE_CPU = 0, PAGE_RAM, PAGE_STORAGE, PAGE_GENERAL, PAGE_PERIPH, PAGE_N };
static const char *k_pages[PAGE_N] =
    { "CPU", "RAM", "STORAGE", "GENERAL", "PERIPH" };

#define GRAPH_BG UI_RGB(0x14, 0x24, 0x36)
#define GRID_COL UI_RGB(0x24, 0x3C, 0x56)

typedef struct {
    char     name[NAME_MAXLEN];
    uint32_t ctr;
} task_snap_t;

typedef struct {
    int  page;
    bool help;
    int64_t last_sample_us;

    /* circular histories (fractions 0..1), index 0 = oldest */
    float cpu_hist[GRAPH_SAMPLES];
    float c0_hist[GRAPH_SAMPLES];
    float c1_hist[GRAPH_SAMPLES];
    float ram_hist[GRAPH_SAMPLES];
    int   fill;

    /* run-time-stats sampling state */
    TaskStatus_t tasks[MAX_TASKS];
    task_snap_t  snap[MAX_TASKS];
    UBaseType_t  snap_n;
    uint64_t     prev_total;
    bool         have_prev;
    uint32_t     ntasks;
    char         top_name[3][12];
    int          top_pct[3];

    /* ram cache */
    uint32_t heap_total, heap_free, heap_min, heap_largest;
} sysmon_t;

/* ------------------------------------------------------------------ */
/* Circular history helpers                                            */
/* ------------------------------------------------------------------ */
static inline void push_hist(float *hist, int *fill, float v)
{
    if (*fill < GRAPH_SAMPLES) {
        hist[(*fill)++] = v;
    } else {
        memmove(hist, hist + 1, sizeof(float) * (GRAPH_SAMPLES - 1));
        hist[GRAPH_SAMPLES - 1] = v;
    }
}

/* Bar-graph painter: newest sample at the right edge, bounded buffers. */
static void draw_graph(ili9488_t *lcd, const ui_rect_t *r,
                       const sysmon_t *st, const float *hist,
                       uint16_t col, const char *label)
{
    ui_draw_fill_rect(lcd, r, GRAPH_BG);
    for (int g = 1; g < 4; g++) {
        ui_rect_t line = { r->x, (uint16_t)(r->y + r->h * g / 4), r->w, 1 };
        ui_draw_fill_rect(lcd, &line, GRID_COL);
    }
    for (int px = 0; px < r->w; px++) {
        int back = r->w - 1 - px;              /* samples ago */
        int pos  = st->fill - 1 - back;        /* position in filled range */
        if (pos < 0) continue;
        int slot = (int)((int64_t)pos * GRAPH_SAMPLES / st->fill);
        float v = hist[slot];
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        int h = (int)(v * (r->h - 2));
        if (h <= 0) continue;
        ui_rect_t bar = { (uint16_t)(r->x + px),
                          (uint16_t)(r->y + r->h - 1 - h), 1, (uint16_t)h };
        ui_draw_fill_rect(lcd, &bar, col);
    }
    if (label) {
        ui_draw_text(lcd, (uint16_t)(r->x + 4), (uint16_t)(r->y + 2),
                     label, UI_C_TEXT_DIM, GRAPH_BG);
    }
}

/* ------------------------------------------------------------------ */
/* Small format helpers                                                */
/* ------------------------------------------------------------------ */
static void fmt_kb(char *out, size_t n, uint64_t bytes)
{
    if (bytes >= 1024u * 1024u) {
        snprintf(out, n, "%.1f MB", (double)bytes / 1048576.0);
    } else {
        snprintf(out, n, "%u KB", (unsigned)(bytes / 1024));
    }
}

static void fmt_uptime(char *out, size_t n, int64_t us)
{
    int64_t s = us / 1000000;
    snprintf(out, n, "%02d:%02d:%02d",
             (int)(s / 3600), (int)((s / 60) % 60), (int)(s % 60));
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    default:                return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Sampling                                                            */
/* ------------------------------------------------------------------ */
static void sysmon_sample(sysmon_t *st)
{
    /* ---- CPU run-time stats ---- */
    UBaseType_t n = uxTaskGetNumberOfTasks();
    if (n > MAX_TASKS) n = MAX_TASKS;
    configRUN_TIME_COUNTER_TYPE total = 0;
    UBaseType_t read = uxTaskGetSystemState(st->tasks, n, &total);

    uint32_t idle_now[2] = { 0, 0 };
    for (UBaseType_t i = 0; i < read; i++) {
        if (!strcmp(st->tasks[i].pcTaskName, "IDLE0")) {
            idle_now[0] = (uint32_t)st->tasks[i].ulRunTimeCounter;
        } else if (!strcmp(st->tasks[i].pcTaskName, "IDLE1")) {
            idle_now[1] = (uint32_t)st->tasks[i].ulRunTimeCounter;
        }
    }
    st->ntasks = (uint32_t)n;

    if (st->have_prev && total > st->prev_total) {
        uint32_t dtotal = (uint32_t)(total - st->prev_total);

        /* utilization traces from per-core idle deltas */
        uint32_t pidle[2] = { 0, 0 };
        for (UBaseType_t j = 0; j < st->snap_n; j++) {
            if (!strcmp(st->snap[j].name, "IDLE0")) pidle[0] = st->snap[j].ctr;
            else if (!strcmp(st->snap[j].name, "IDLE1")) pidle[1] = st->snap[j].ctr;
        }
        if ((pidle[0] || pidle[1]) && dtotal > 0) {
            float did0 = (float)(idle_now[0] - pidle[0]);
            float did1 = (float)(idle_now[1] - pidle[1]);
            float all  = 1.0f - (did0 + did1) / (2.0f * (float)dtotal);
            float c0   = 1.0f - did0 / (float)dtotal;
            float c1   = 1.0f - did1 / (float)dtotal;
            if (all < 0) { all = 0; }
            if (all > 1) { all = 1; }
            if (c0 < 0)  { c0 = 0; }
            if (c0 > 1)  { c0 = 1; }
            if (c1 < 0)  { c1 = 0; }
            if (c1 > 1)  { c1 = 1; }
            push_hist(st->cpu_hist, &st->fill, all);
            push_hist(st->c0_hist, &st->fill, c0);
            push_hist(st->c1_hist, &st->fill, c1);

            float htot = (float)heap_caps_get_total_size(MALLOC_CAP_INTERNAL |
                                                         MALLOC_CAP_8BIT);
            if (htot > 0) {
                float used =
                    1.0f - (float)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                          MALLOC_CAP_8BIT) / htot;
                if (used < 0) { used = 0; }
                if (used > 1) { used = 1; }
                push_hist(st->ram_hist, &st->fill, used);
            }
        }

        /* top-3 busy tasks by runtime delta vs previous snapshot */
        typedef struct { const char *nm; uint32_t dt; } top_t;
        top_t tops[3] = { { NULL, 0 }, { NULL, 0 }, { NULL, 0 } };
        for (UBaseType_t i = 0; i < read; i++) {
            const char *nm = st->tasks[i].pcTaskName;
            if (!strcmp(nm, "IDLE0") || !strcmp(nm, "IDLE1")) continue;
            uint32_t prev = 0;
            bool found = false;
            for (UBaseType_t j = 0; j < st->snap_n; j++) {
                if (!strncmp(st->snap[j].name, nm, NAME_MAXLEN)) {
                    prev = st->snap[j].ctr;
                    found = true;
                    break;
                }
            }
            if (!found) continue;
            uint32_t d = (uint32_t)st->tasks[i].ulRunTimeCounter - prev;
            for (int k = 2; k >= 0; k--) {
                if (d > tops[k].dt) {
                    if (k < 2) tops[k + 1] = tops[k];
                    tops[k].nm = nm;
                    tops[k].dt = d;
                    break;
                }
            }
        }
        for (int k = 0; k < 3; k++) {
            st->top_name[k][0] = 0;
            st->top_pct[k] = 0;
            if (tops[k].nm && dtotal > 0) {
                snprintf(st->top_name[k], sizeof(st->top_name[k]), "%s",
                         tops[k].nm);
                st->top_pct[k] = (int)(100.0f * tops[k].dt / (float)dtotal);
            }
        }
    }

    /* save snapshot for the next delta pass */
    st->snap_n = 0;
    for (UBaseType_t i = 0; i < read && st->snap_n < MAX_TASKS; i++) {
        snprintf(st->snap[st->snap_n].name, NAME_MAXLEN, "%s",
                 st->tasks[i].pcTaskName);
        st->snap[st->snap_n].ctr = (uint32_t)st->tasks[i].ulRunTimeCounter;
        st->snap_n++;
    }
    st->prev_total = total;
    st->have_prev  = true;

    /* ---- RAM ---- */
    st->heap_total   = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    st->heap_free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    st->heap_min     = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    st->heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */
static ui_rect_t tab_rect(const ui_rect_t *c, int i)
{
    int tw = 88;
    int gap = 4;
    int x = c->x + 4 + i * (tw + gap);
    ui_rect_t r = { (uint16_t)x, c->y, (uint16_t)tw, 18 };
    return r;
}

/* ------------------------------------------------------------------ */
/* Pages                                                               */
/* ------------------------------------------------------------------ */
static void row_text(ili9488_t *lcd, int x, int y, const char *label,
                     const char *val)
{
    ui_draw_text(lcd, (uint16_t)x, (uint16_t)y, label, UI_C_TEXT_DIM,
                 UI_C_WIN_BG);
    ui_draw_text(lcd, (uint16_t)(x + 150), (uint16_t)y, val, UI_C_TEXT,
                 UI_C_WIN_BG);
}

static void page_cpu(ili9488_t *lcd, const ui_rect_t *c, const sysmon_t *st)
{
    char b[96];
    float all = st->fill ? st->cpu_hist[st->fill - 1] : 0;
    float l0  = st->fill ? st->c0_hist[st->fill - 1] : 0;
    float l1  = st->fill ? st->c1_hist[st->fill - 1] : 0;
    int y = c->y + 24;

    snprintf(b, sizeof(b), "Load %3d%%   Core0 %3d%%   Core1 %3d%%   Freq %u MHz   Tasks %u",
             (int)(all * 100), (int)(l0 * 100), (int)(l1 * 100),
             (unsigned)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
             (unsigned)st->ntasks);
    ui_draw_text(lcd, (uint16_t)(c->x + 6), (uint16_t)y, b, UI_C_TEXT,
                 UI_C_WIN_BG);

    ui_rect_t g = { (uint16_t)(c->x + 6), (uint16_t)(y + 20),
                    (uint16_t)(c->w - 12), 100 };
    draw_graph(lcd, &g, st, st->cpu_hist, UI_C_ACCENT, "CPU UTILIZATION");

    int hw = (c->w - 12 - 8) / 2;
    ui_rect_t g0 = { (uint16_t)(c->x + 6), (uint16_t)(y + 128),
                     (uint16_t)hw, 52 };
    ui_rect_t g1 = { (uint16_t)(c->x + 14 + hw), (uint16_t)(y + 128),
                     (uint16_t)hw, 52 };
    draw_graph(lcd, &g0, st, st->c0_hist, UI_RGB(0x5A, 0xC8, 0x6A), "CORE 0");
    draw_graph(lcd, &g1, st, st->c1_hist, UI_RGB(0xE8, 0x99, 0x2A), "CORE 1");

    snprintf(b, sizeof(b), "Top: %s %d%%   %s %d%%   %s %d%%",
             st->top_name[0], st->top_pct[0],
             st->top_name[1], st->top_pct[1],
             st->top_name[2], st->top_pct[2]);
    ui_draw_text(lcd, (uint16_t)(c->x + 6), (uint16_t)(y + 190), b,
                 UI_C_TEXT_DIM, UI_C_WIN_BG);
}

static void page_ram(ili9488_t *lcd, const ui_rect_t *c, const sysmon_t *st)
{
    char b1[32], b2[32], b3[32];
    int y = c->y + 24;

    fmt_kb(b1, sizeof(b1), st->heap_total);
    fmt_kb(b2, sizeof(b2), st->heap_free);
    snprintf(b3, sizeof(b3), "%d%%",
             st->heap_total ? (int)(100 - 100.0f * st->heap_free / st->heap_total) : 0);
    row_text(lcd, c->x + 6, y, "Heap used:", b3);
    ui_draw_text(lcd, (uint16_t)(c->x + 230), (uint16_t)y,
                 "(internal)", UI_C_TEXT_DIM, UI_C_WIN_BG);

    row_text(lcd, c->x + 6, y + 16, "Total:", b1);
    row_text(lcd, c->x + 6, y + 32, "Free:", b2);
    fmt_kb(b1, sizeof(b1), st->heap_total - st->heap_free);
    row_text(lcd, c->x + 230, y + 16, "Used:", b1);
    fmt_kb(b1, sizeof(b1), st->heap_min);
    row_text(lcd, c->x + 230, y + 32, "Min ever:", b1);
    fmt_kb(b1, sizeof(b1), st->heap_largest);
    row_text(lcd, c->x + 6, y + 48, "Largest block:", b1);
    ui_draw_text(lcd, (uint16_t)(c->x + 230), (uint16_t)(y + 48),
                 "PSRAM: N/A", UI_C_TEXT_DIM, UI_C_WIN_BG);

    ui_rect_t g = { (uint16_t)(c->x + 6), (uint16_t)(y + 74),
                    (uint16_t)(c->w - 12), 130 };
    draw_graph(lcd, &g, st, st->ram_hist, UI_RGB(0xE8, 0x5A, 0x87),
               "HEAP USED %");
}

static void page_storage(ili9488_t *lcd, const ui_rect_t *c, const sysmon_t *st)
{
    (void)st;
    char b[64];
    int y = c->y + 24;

    ui_draw_text(lcd, (uint16_t)(c->x + 6), (uint16_t)y,
                 "PARTITIONS (internal flash)", UI_C_TEXT_DIM, UI_C_WIN_BG);
    y += 16;
    ui_draw_text(lcd, (uint16_t)(c->x + 6), (uint16_t)y,
                 "NAME       KIND     OFFSET     SIZE", UI_C_TEXT_DIM,
                 UI_C_WIN_BG);
    y += 15;

    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    int rows = 0;
    while (it && rows < 6) {
        const esp_partition_t *p = esp_partition_get(it);
        const char *kind =
            p->type == ESP_PARTITION_TYPE_APP ? "app" :
            p->type == ESP_PARTITION_TYPE_DATA ? "data" : "?";
        snprintf(b, sizeof(b), "%-10s %-8s 0x%06X   %u KB",
                 p->label, kind, (unsigned)p->address,
                 (unsigned)(p->size / 1024));
        ui_draw_text(lcd, (uint16_t)(c->x + 6), (uint16_t)y, b,
                     UI_C_TEXT, UI_C_WIN_BG);
        y += 15;
        rows++;
        it = esp_partition_next(it);
    }
    if (it) {
        esp_partition_iterator_release(it);
    }

    const esp_partition_t *run = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "factory");
    if (run) {
        fmt_kb(b, sizeof(b) - 24, run->size);
        strcat(b, " (app)");
        row_text(lcd, c->x + 6, y + 4, "App partition:", b);
    }
    y += 24;

    ui_draw_text(lcd, (uint16_t)(c->x + 6), (uint16_t)y, "SD CARD (FAT)",
                 UI_C_TEXT_DIM, UI_C_WIN_BG);
    y += 15;
    struct statvfs vfs;
    if (statvfs("/sdcard", &vfs) == 0) {
        uint64_t tot = (uint64_t)vfs.f_blocks * vfs.f_frsize;
        uint64_t fre = (uint64_t)vfs.f_bfree * vfs.f_frsize;
        fmt_kb(b, sizeof(b), tot);
        ui_draw_text(lcd, (uint16_t)(c->x + 6), (uint16_t)y, "Mounted, capacity:",
                     UI_C_TEXT_DIM, UI_C_WIN_BG);
        ui_draw_text(lcd, (uint16_t)(c->x + 150), (uint16_t)y, b,
                     UI_C_TEXT, UI_C_WIN_BG);
        fmt_kb(b, sizeof(b), fre);
        ui_draw_text(lcd, (uint16_t)(c->x + 6), (uint16_t)(y + 15), "Free:",
                     UI_C_TEXT_DIM, UI_C_WIN_BG);
        ui_draw_text(lcd, (uint16_t)(c->x + 150), (uint16_t)(y + 15), b,
                     UI_C_TEXT, UI_C_WIN_BG);
    } else {
        ui_draw_text(lcd, (uint16_t)(c->x + 6), (uint16_t)y,
                     "Not mounted", UI_C_TEXT, UI_C_WIN_BG);
    }
}

static void page_general(ili9488_t *lcd, const ui_rect_t *c, const sysmon_t *st)
{
    char b[64];
    int y = c->y + 24;
    const esp_app_desc_t *app = esp_app_get_description();

    row_text(lcd, c->x + 6, y, "Arpile firmware:", app->project_name); y += 16;
    row_text(lcd, c->x + 6, y, "Version:", app->version); y += 16;
    row_text(lcd, c->x + 6, y, "ESP-IDF:", esp_get_idf_version()); y += 16;

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    snprintf(b, sizeof(b), "ESP32-P4 rev %u, %u cores",
             (unsigned)chip.revision, (unsigned)chip.cores);
    row_text(lcd, c->x + 6, y, "Chip:", b); y += 16;

    snprintf(b, sizeof(b), "%u MHz",
             (unsigned)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    row_text(lcd, c->x + 6, y, "CPU clock:", b); y += 16;

    fmt_uptime(b, sizeof(b), esp_timer_get_time());
    row_text(lcd, c->x + 6, y, "Uptime:", b); y += 16;

    row_text(lcd, c->x + 6, y, "Last reset:",
             reset_reason_str(esp_reset_reason())); y += 16;

    time_t now = time(NULL);
    if (now > 1000000000) {
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tmv);
    } else {
        snprintf(b, sizeof(b), "clock not set");
    }
    row_text(lcd, c->x + 6, y, "Date/time:", b);
    (void)st;
}

static void page_periph(ili9488_t *lcd, const ui_rect_t *c, const sysmon_t *st)
{
    char b[80];
    int y = c->y + 24;

    row_text(lcd, c->x + 6, y, "Display:", "ILI9488 480x320 SPI — OK"); y += 18;

    row_text(lcd, c->x + 6, y, "Audio:", "N/A"); y += 18;

    row_text(lcd, c->x + 6, y, "USB HID:", "N/A"); y += 18;

    const char *wst;
    switch (arpile_wifi_get_state()) {
    case ARPILE_WIFI_CONNECTED:   wst = "connected"; break;
    case ARPILE_WIFI_CONNECTING:  wst = "connecting"; break;
    default:                      wst = "disconnected"; break;
    }
    snprintf(b, sizeof(b), "%s  SSID \"%s\"  IP %s", wst,
             arpile_wifi_get_connected_ssid(), arpile_wifi_get_ip());
    row_text(lcd, c->x + 6, y, "Network:", b); y += 18;

    if (statvfs("/sdcard", &(struct statvfs){ 0 }) == 0) {
        snprintf(b, sizeof(b), "mounted at /sdcard");
    } else {
        snprintf(b, sizeof(b), "not mounted");
    }
    row_text(lcd, c->x + 6, y, "SD card:", b);
    (void)st;
}

/* ------------------------------------------------------------------ */
/* App glue                                                            */
/* ------------------------------------------------------------------ */
static void draw_tabs(ili9488_t *lcd, const ui_rect_t *c, const sysmon_t *st)
{
    for (int i = 0; i < PAGE_N; i++) {
        ui_rect_t r = tab_rect(c, i);
        ui_draw_fill_rect(lcd, &r,
                          i == st->page ? UI_C_TASK_ACT : UI_C_BUTTON);
        ui_draw_outline(lcd, &r, UI_C_BORDER);
        uint16_t tw = ui_text_width(k_pages[i]);
        ui_draw_text(lcd, (uint16_t)(r.x + (r.w - tw) / 2),
                     (uint16_t)(r.y + 3), k_pages[i],
                     i == st->page ? UI_RGB(0xFF, 0xFF, 0xFF) : UI_C_TEXT_DIM,
                     i == st->page ? UI_C_TASK_ACT : UI_C_BUTTON);
    }
}

static void draw_help(ili9488_t *lcd, const ui_rect_t *c)
{
    ui_rect_t box = { (uint16_t)(c->x + 40), (uint16_t)(c->y + 30),
                      (uint16_t)(c->w - 80), 150 };
    ui_draw_fill_rect(lcd, &box, UI_C_WIN_BG);
    ui_draw_outline(lcd, &box, UI_C_ACCENT);
    int x = box.x + 12, y = box.y + 10;
    static const char *lines[] = {
        "SYSMON HELP",
        "Tab / Shift+Tab   next / previous page",
        "Left / Right      previous / next page",
        "Click a tab       jump to that page",
        "F1                toggle this help",
        "Esc               close Sysmon",
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        ui_draw_text(lcd, (uint16_t)x, (uint16_t)y, lines[i],
                     i == 0 ? UI_C_ACCENT : UI_C_TEXT, UI_C_WIN_BG);
        y += 20;
    }
}

static void sysmon_render(arpile_app_ctx_t *ctx, ui_win_t *win)
{
    sysmon_t *st = ctx->user;
    if (!st) return;
    ili9488_t *lcd = arpile_ui_get_lcd();
    ui_rect_t c = win_client_rect(win);

    ui_draw_fill_rect(lcd, &c, UI_C_WIN_BG);
    draw_tabs(lcd, &c, st);

    ui_rect_t body = c;
    body.y += 22;
    body.h -= 22;
    switch (st->page) {
    case PAGE_CPU:     page_cpu(lcd, &body, st); break;
    case PAGE_RAM:     page_ram(lcd, &body, st); break;
    case PAGE_STORAGE: page_storage(lcd, &body, st); break;
    case PAGE_GENERAL: page_general(lcd, &body, st); break;
    case PAGE_PERIPH:  page_periph(lcd, &body, st); break;
    }

    if (st->help) {
        draw_help(lcd, &c);
    }
}

static void sysmon_update(arpile_app_ctx_t *ctx)
{
    sysmon_t *st = ctx->user;
    if (!st || !ctx->win) return;

    int64_t now = esp_timer_get_time();
    if (now - st->last_sample_us < SAMPLE_US) return;
    sysmon_sample(st);
    arpile_ui_win_redraw(ctx->win);
}

static void sysmon_event(arpile_app_ctx_t *ctx, const arpile_input_event_t *ev)
{
    sysmon_t *st = ctx->user;
    if (!st) return;

    if (ev->type == ARPILE_IN_EVENT_KEY_DOWN) {
        uint16_t k = ev->key.keycode;
        uint8_t shift = ev->key.modifier &
                        (ARPILE_MOD_LSHIFT | ARPILE_MOD_RSHIFT);
        if (k == ARPILE_KEY_F1) {
            st->help = !st->help;
            arpile_ui_win_redraw(ctx->win);
            return;
        }
        if (k == ARPILE_KEY_TAB) {
            st->page = (st->page + (shift ? PAGE_N - 1 : 1)) % PAGE_N;
            arpile_ui_win_redraw(ctx->win);
            return;
        }
        if (k == ARPILE_KEY_LEFT) {
            st->page = (st->page + PAGE_N - 1) % PAGE_N;
            arpile_ui_win_redraw(ctx->win);
            return;
        }
        if (k == ARPILE_KEY_RIGHT) {
            st->page = (st->page + 1) % PAGE_N;
            arpile_ui_win_redraw(ctx->win);
            return;
        }
    } else if (ev->type == ARPILE_IN_EVENT_MOUSE_BTN &&
               (ev->mouse.buttons & ARPILE_MOUSE_BTN_LEFT)) {
        ili9488_t *lcd = arpile_ui_get_lcd();
        ui_rect_t c = win_client_rect(ctx->win);
        if (st->help) {
            st->help = false;
            arpile_ui_win_redraw(ctx->win);
            return;
        }
        for (int i = 0; i < PAGE_N; i++) {
            ui_rect_t r = tab_rect(&c, i);
            if (ui_rect_contains(&r, ev->mouse.x, ev->mouse.y)) {
                if (st->page != i) {
                    st->page = i;
                    arpile_ui_win_redraw(ctx->win);
                }
                return;
            }
        }
        (void)lcd;
    }
}

static void sysmon_init(arpile_app_ctx_t *ctx)
{
    sysmon_t *st = calloc(1, sizeof(sysmon_t));
    if (!st) return;
    ctx->user = st;
    sysmon_sample(st);
    st->last_sample_us = esp_timer_get_time();
}

static void sysmon_destroy(arpile_app_ctx_t *ctx)
{
    free(ctx->user);
    ctx->user = NULL;
}

static const arpile_app_ops_t sysmon_ops = {
    .init = sysmon_init,
    .update = sysmon_update,
    .event = sysmon_event,
    .render = sysmon_render,
    .destroy = sysmon_destroy,
};

const arpile_app_t arpile_app_sysmon = {
    .id = "sysmon",
    .name = "Sysmon",
    .icon = "sysmon",
    .ops = &sysmon_ops,
};
