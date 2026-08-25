/* Arpile Files: file manager for the /sdcard FATFS volume.
 * Browse, navigate, inspect metadata; keyboard-first with pointer support. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include "arpile_app.h"
#include "arpile_ui.h"
#include "arpile_ui_widgets.h"
#include "usb_host_input.h"

#define FILES_ROOT     "/sdcard"
#define FILES_MAX      96
#define FILES_NAME_MAX 64
#define FILES_ROW_H    (CHAR_H + 6)

typedef struct {
    char name[FILES_NAME_MAX];
    bool is_dir;
    uint32_t size;
} files_entry_t;

typedef struct {
    char cwd[128];
    files_entry_t entries[FILES_MAX];
    int count;
    int sel;
    int top;
    char msg[64];
} files_state_t;

static const char *k_dirs_first = ".. (parent)";

static void fmt_size(char *out, size_t n, uint32_t bytes)
{
    if (bytes >= 1024 * 1024) {
        snprintf(out, n, "%lu.%luM", (unsigned long)(bytes / (1024 * 1024)),
                 (unsigned long)((bytes % (1024 * 1024)) * 10 / (1024 * 1024)));
    } else if (bytes >= 1024) {
        snprintf(out, n, "%luK", (unsigned long)(bytes / 1024));
    } else {
        snprintf(out, n, "%luB", (unsigned long)bytes);
    }
}

static int entry_cmp(const void *a, const void *b)
{
    const files_entry_t *ea = a, *eb = b;
    if (ea->is_dir != eb->is_dir) {
        return ea->is_dir ? -1 : 1;
    }
    return strcasecmp(ea->name, eb->name);
}

/* List viewport inside the window client rect. */
static ui_rect_t list_rect(const ui_rect_t *c)
{
    ui_rect_t r = { (uint16_t)(c->x + 2), (uint16_t)(c->y + CHAR_H + 8),
                    (uint16_t)(c->w - 4),
                    (uint16_t)(c->h - CHAR_H - 8 - CHAR_H - 12) };
    return r;
}

static void files_reload(files_state_t *st)
{
    st->count = 0;
    st->sel = 0;
    st->top = 0;

    DIR *dir = opendir(st->cwd);
    if (!dir) {
        snprintf(st->msg, sizeof(st->msg), "Cannot open %.40s", st->cwd);
        return;
    }
    struct dirent *ent;
    while (st->count < FILES_MAX && (ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        files_entry_t *e = &st->entries[st->count++];
        snprintf(e->name, sizeof(e->name), "%.60s", ent->d_name);
        char path[192];
        snprintf(path, sizeof(path), "%s/%s", st->cwd, e->name);
        struct stat sb;
        if (stat(path, &sb) == 0) {
            e->is_dir = S_ISDIR(sb.st_mode);
            e->size = e->is_dir ? 0 : (uint32_t)sb.st_size;
        } else {
            e->is_dir = false;
            e->size = 0;
        }
    }
    closedir(dir);
    qsort(st->entries, st->count, sizeof(files_entry_t), entry_cmp);

    if (strcmp(st->cwd, FILES_ROOT) != 0) {
        /* parent shortcut row at top */
        memmove(&st->entries[1], &st->entries[0],
                sizeof(files_entry_t) * (FILES_MAX - 1));
        if (st->count < FILES_MAX) st->count++;
        files_entry_t *up = &st->entries[0];
        snprintf(up->name, sizeof(up->name), "..");
        up->is_dir = true;
        up->size = 0;
    }
}

static void join_path(char *dst, size_t n, const char *dir, const char *name)
{
    if (strcmp(dir, "/") == 0) {
        snprintf(dst, n, "/%s", name);
    } else {
        snprintf(dst, n, "%s/%s", dir, name);
    }
}

static void open_entry(arpile_app_ctx_t *ctx, files_state_t *st, int idx)
{
    if (idx < 0 || idx >= st->count) return;
    files_entry_t *e = &st->entries[idx];
    char path[192];

    if (strcmp(e->name, "..") == 0) {
        char *slash = strrchr(st->cwd, '/');
        if (slash && slash != st->cwd) {
            *slash = 0;
        } else {
            strcpy(st->cwd, FILES_ROOT);
        }
        files_reload(st);
        arpile_ui_win_redraw(ctx->win);
        return;
    }

    join_path(path, sizeof(path), st->cwd, e->name);
    if (e->is_dir) {
        strncpy(st->cwd, path, sizeof(st->cwd) - 1);
        st->cwd[sizeof(st->cwd) - 1] = 0;
        files_reload(st);
        arpile_ui_win_redraw(ctx->win);
        return;
    }

    /* File: show metadata until a dedicated viewer/editor opens it. */
    char size[16];
    fmt_size(size, sizeof(size), e->size);
    const char *ext = strrchr(e->name, '.');
    ext = ext ? ext + 1 : "";
    const char *kind =
        !strcasecmp(ext, "txt") || !strcasecmp(ext, "md") ? "text" :
        !strcasecmp(ext, "bmp") || !strcasecmp(ext, "jpg") ? "image" :
        !strcasecmp(ext, "wav") ? "audio" : "binary";
    snprintf(st->msg, sizeof(st->msg), "%.6s: %.7s %.40s",
             kind, size, e->name);
    arpile_ui_win_redraw(ctx->win);
}

/* ---------------- render ---------------- */

static const char *files_row_label(int index, void *user)
{
    files_state_t *st = user;
    static char row[80];   /* single-threaded UI; safe */
    if (index < 0 || index >= st->count) return NULL;
    files_entry_t *e = &st->entries[index];
    char size[16] = "";
    if (!e->is_dir && strcmp(e->name, "..") != 0) {
        fmt_size(size, sizeof(size), e->size);
    }
    if (index == 0 && strcmp(e->name, "..") == 0 &&
        strcmp(st->cwd, FILES_ROOT) != 0) {
        snprintf(row, sizeof(row), "%.40s", k_dirs_first);
    } else {
        snprintf(row, sizeof(row), "%s%.44s%s%s",
                 e->is_dir ? "[" : "", e->name,
                 e->is_dir ? "]" : "", size[0] ? "  " : "");
        if (size[0]) {
            size_t len = strlen(row);
            snprintf(row + len, sizeof(row) - len, "%s", size);
        }
    }
    return row;
}

static void files_render(arpile_app_ctx_t *ctx, ui_win_t *win)
{
    files_state_t *st = ctx->user;
    if (!st) return;
    ili9488_t *lcd = arpile_ui_get_lcd();
    ui_rect_t c = win_client_rect(win);
    ui_draw_fill_rect(lcd, &c, UI_C_WIN_BG);

    /* path bar */
    char title[96];
    snprintf(title, sizeof(title), "%.70s (%d item%s)", st->cwd, st->count,
             st->count == 1 ? "" : "s");
    ui_draw_text(lcd, (uint16_t)(c.x + 6), (uint16_t)(c.y + 5), title,
                 UI_C_TEXT_DIM, UI_C_WIN_BG);

    ui_rect_t listr = list_rect(&c);
    ui_kit_list_paint(lcd, &listr, FILES_ROW_H, st->top, st->sel, st->count,
                      files_row_label, st);

    char left[72], right[24] = "";
    if (st->count > 0 && st->sel < st->count) {
        files_entry_t *e = &st->entries[st->sel];
        if (e->is_dir) {
            snprintf(left, sizeof(left), "[dir] %.60s", e->name);
        } else {
            char size[16];
            fmt_size(size, sizeof(size), e->size);
            snprintf(left, sizeof(left), "%.50s (%s)", e->name, size);
        }
    } else {
        snprintf(left, sizeof(left), "Empty directory");
    }
    snprintf(right, sizeof(right), "%d/%d", st->count ? st->sel + 1 : 0,
             st->count);
    ui_kit_statusbar(lcd, &c, left, right);
}

/* ---------------- events ---------------- */

/* NOTE: arrow keys are consumed by the desktop as mouse-cursor movement and
 * never reach apps. Navigation follows the WiFi app convention:
 * PgUp/PgDn = one row, Shift+PgUp/PgDn = one page, Home/End = jump. */
static void files_event(arpile_app_ctx_t *ctx, const arpile_input_event_t *ev)
{
    files_state_t *st = ctx->user;
    if (!st) return;

    if (ev->type == ARPILE_IN_EVENT_KEY_DOWN) {
        switch (ev->key.keycode) {
        case ARPILE_KEY_PGUP:
        case ARPILE_KEY_PGDN: {
            uint8_t m = ev->key.modifier;
            bool shift = (m & (ARPILE_MOD_LSHIFT | ARPILE_MOD_RSHIFT)) != 0;
            ui_rect_t c = win_client_rect(ctx->win);
            ui_rect_t listr = list_rect(&c);
            int view_rows = (listr.h - 2) / FILES_ROW_H;
            if (view_rows < 1) view_rows = 1;

            if (shift) {
                /* whole page */
                ui_kit_list_nav(&st->sel, &st->top, view_rows, st->count,
                                ev->key.keycode);
            } else {
                /* one row (WiFi app convention) */
                st->sel += (ev->key.keycode == ARPILE_KEY_PGDN) ? 1 : -1;
                if (st->sel < 0) st->sel = 0;
                if (st->sel > st->count - 1) st->sel = st->count - 1;
                if (st->sel < st->top) {
                    st->top = st->sel;
                } else if (st->sel > st->top + view_rows - 1) {
                    st->top = st->sel - view_rows + 1;
                }
            }
            arpile_ui_win_redraw(ctx->win);
            return;
        }
        case ARPILE_KEY_HOME:
        case ARPILE_KEY_END: {
            ui_rect_t c = win_client_rect(ctx->win);
            ui_rect_t listr = list_rect(&c);
            int view_rows = (listr.h - 2) / FILES_ROW_H;
            if (view_rows < 1) view_rows = 1;
            ui_kit_list_nav(&st->sel, &st->top, view_rows, st->count,
                            ev->key.keycode);
            arpile_ui_win_redraw(ctx->win);
            return;
        }
        case ARPILE_KEY_ENTER:
            open_entry(ctx, st, st->sel);
            return;
        case ARPILE_KEY_BACKSPACE:
        case ARPILE_KEY_ESCAPE:
            if (ev->key.keycode == ARPILE_KEY_ESCAPE &&
                strcmp(st->cwd, FILES_ROOT) == 0) {
                arpile_app_close(ctx);
            } else {
                open_entry(ctx, st, 0);   /* ".." is row 0 when not at root */
            }
            return;
        default:
            return;
        }
    }

    if (ev->type == ARPILE_IN_EVENT_MOUSE_BTN) {
        bool pressed = (ev->mouse.buttons & ARPILE_MOUSE_BTN_LEFT) != 0;
        if (!pressed) return;
        ui_rect_t c = win_client_rect(ctx->win);
        ui_rect_t listr = list_rect(&c);
        int rel_y = ev->mouse.y - (int)listr.y - 2;
        if (rel_y >= 0) {
            int row = rel_y / FILES_ROW_H;
            int idx = st->top + row;
            if (idx >= 0 && idx < st->count) {
                if (idx == st->sel) {
                    open_entry(ctx, st, idx);          /* click again opens */
                } else {
                    st->sel = idx;                     /* first click selects */
                }
                arpile_ui_win_redraw(ctx->win);
            }
        }
    }
}

static void files_init(arpile_app_ctx_t *ctx)
{
    files_state_t *st = calloc(1, sizeof(files_state_t));
    ctx->user = st;
    strcpy(st->cwd, FILES_ROOT);
    files_reload(st);
}

static void files_destroy(arpile_app_ctx_t *ctx)
{
    if (ctx->user) {
        free(ctx->user);
        ctx->user = NULL;
    }
}

static const arpile_app_ops_t files_ops = {
    .init = files_init,
    .event = files_event,
    .render = files_render,
    .destroy = files_destroy,
};

const arpile_app_t arpile_app_files_mgr = {
    .id = "files",
    .name = "Files",
    .icon = "files2",
    .ops = &files_ops,
};
