/* Arpile Code: a miniature VS Code-style IDE for the ESP32-P4.
 *
 * Layout: tabs row (top), explorer | editor (split), output panel (bottom).
 * Reuses the desktop window manager, input system, FATFS APIs, and the
 * terminal's command semantics for Ctrl+P. Closes cleanly through the
 * deferred-teardown architecture so the launcher keeps receiving mouse clicks.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "arpile_app.h"
#include "arpile_ui.h"
#include "arpile_ui_widgets.h"
#include "usb_host_input.h"
#include "ili9488.h"

#define CODE_ROOT        "/sdcard"
#define CODE_FILES_MAX   96
#define CODE_TEXT_MAX    32768
#define CODE_MAX_TABS    8
#define CODE_UNDO_MAX    128
#define CODE_CLIP_MAX    8192
#define CODE_OUT_MAX     4096
#define CODE_EXPL_W      134
#define CODE_TAB_H       16
#define CODE_OUT_H       68
#define CODE_LH          14
#define CODE_GUTTER      (CHAR_W * 4)

#define CODE_MODE_BROWSE 0
#define CODE_MODE_EDIT   1

enum { LANG_NONE=0, LANG_C, LANG_CPP, LANG_PY, LANG_RUST, LANG_JS, LANG_JSON, LANG_SH };

enum { PANE_EDITOR=0, PANE_EXPLORER, PANE_OUTPUT };
enum { PMPT_NONE=0, PMPT_CMD, PMPT_FIND, PMPT_SAVEAS, PMPT_RENAME,
       PMPT_NEWFILE, PMPT_NEWFOLDER };

/* Highlight colors (RGB565) */
#define HL_TEXT   UI_C_TEXT
#define HL_KW     UI_RGB(0x6A,0xA0,0xE8)
#define HL_STR    UI_RGB(0x7E,0xC0,0x60)
#define HL_NUM    UI_RGB(0xE6,0xC0,0x7A)
#define HL_COM    UI_C_TEXT_DIM
#define HL_PRE    UI_RGB(0xC0,0x8A,0xE0)

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */
typedef struct { int pos; int len; bool is_insert; char *data; } code_undo_t;

typedef struct {
    char path[200];
    char *text;
    int len, cap;
    int cursor, anchor;
    int scroll_y, scroll_x;
    bool modified;
    int lang;
    bool in_block;
    code_undo_t *undo;
    int undo_n;
    code_undo_t *redo;
    int redo_n;
} code_tab_t;

typedef struct code_node {
    char name[64];
    char path[256];
    bool is_dir;
    uint32_t size;
    bool expanded;
    bool loaded;
    struct code_node *children;
    int child_count;
    struct code_node *parent;
} code_node_t;

typedef struct {
    char buf[CODE_OUT_MAX];
    int len;
    int scroll;   /* lines scrolled up from the bottom (0 = bottom) */
} code_out_t;

typedef struct {
    code_node_t *root;
    code_node_t **vis;
    int vis_n, vis_cap;
    int expl_sel;
    int expl_scroll;
    int expl_rows;
    code_tab_t tabs[CODE_MAX_TABS];
    int tab_n, tab_active;
    int pane;
    int prompt;
    char input[128];
    int input_pos;
    char prompt_label[24];
    code_node_t *rename_node;
    char exec_cwd[256];
    char msg[96];
    code_out_t out;
    bool out_open;
} code_state_t;

static char g_clip[CODE_CLIP_MAX];
static int g_clip_len = 0;

/* ------------------------------------------------------------------ */
/* Output panel buffer                                                 */
/* ------------------------------------------------------------------ */
static void out_append(code_state_t *s, const char *txt)
{
    int add = strlen(txt);
    if (add <= 0) return;
    if (s->out.len + add + 1 > CODE_OUT_MAX) {
        int drop = (s->out.len + add + 1) - CODE_OUT_MAX;
        memmove(s->out.buf, s->out.buf + drop, s->out.len - drop);
        s->out.len -= drop;
    }
    memcpy(s->out.buf + s->out.len, txt, add);
    s->out.len += add;
    s->out.buf[s->out.len] = 0;
}
static void out_appendln(code_state_t *s, const char *txt)
{
    out_append(s, txt);
    out_append(s, "\n");
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static const char *base_name(const char *p)
{
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}
static void code_lang_for(const char *name, int *lang)
{
    const char *p = strrchr(name, '.');
    if (!p) { *lang = LANG_NONE; return; }
    if (!strcasecmp(p, ".c") || !strcasecmp(p, ".h")) *lang = LANG_C;
    else if (!strcasecmp(p, ".cpp") || !strcasecmp(p, ".cc") ||
             !strcasecmp(p, ".cxx") || !strcasecmp(p, ".hpp")) *lang = LANG_CPP;
    else if (!strcasecmp(p, ".py")) *lang = LANG_PY;
    else if (!strcasecmp(p, ".rs")) *lang = LANG_RUST;
    else if (!strcasecmp(p, ".js") || !strcasecmp(p, ".mjs")) *lang = LANG_JS;
    else if (!strcasecmp(p, ".json")) *lang = LANG_JSON;
    else if (!strcasecmp(p, ".sh")) *lang = LANG_SH;
    else *lang = LANG_NONE;
}

/* ------------------------------------------------------------------ */
/* Per-tab buffer + undo                                               */
/* ------------------------------------------------------------------ */
static bool tab_ensure(code_tab_t *t, int need)
{
    if (t->len + need + 1 <= t->cap) return true;
    int ncap = t->cap ? t->cap * 2 : 1024;
    while (ncap < t->len + need + 1) ncap *= 2;
    if (ncap > CODE_TEXT_MAX) ncap = CODE_TEXT_MAX;
    if (t->len + need + 1 > ncap) return false;
    char *nb = realloc(t->text, ncap);
    if (!nb) return false;
    t->text = nb; t->cap = ncap;
    return true;
}
static void tab_insert(code_tab_t *t, int pos, const char *p, int n)
{
    if (n <= 0) return;
    if (pos < 0) pos = 0;
    if (pos > t->len) pos = t->len;
    if (!tab_ensure(t, n)) return;
    memmove(t->text + pos + n, t->text + pos, t->len - pos);
    memcpy(t->text + pos, p, n);
    t->len += n;
    t->text[t->len] = 0;
    if (t->cursor >= pos) t->cursor += n;
    if (t->anchor >= pos) t->anchor += n;
}
static void tab_delete(code_tab_t *t, int pos, int n)
{
    if (n <= 0) return;
    if (pos < 0) pos = 0;
    if (pos + n > t->len) n = t->len - pos;
    if (n <= 0) return;
    memmove(t->text + pos, t->text + pos + n, t->len - pos - n);
    t->len -= n;
    t->text[t->len] = 0;
    if (t->cursor > pos) {
        t->cursor = (t->cursor > pos + n) ? t->cursor - n : pos;
    }
    if (t->anchor > pos) {
        t->anchor = (t->anchor > pos + n) ? t->anchor - n : pos;
    }
}
static void undo_clear(code_undo_t *stk, int *n)
{
    while (*n) { (*n)--; free(stk[*n].data); stk[*n].data = NULL; }
}
static void push_undo(code_tab_t *t, bool is_insert, int pos, const char *data, int n)
{
    if (!t->undo || !t->redo) return;
    if (t->undo_n >= CODE_UNDO_MAX) {
        free(t->undo[0].data);
        memmove(&t->undo[0], &t->undo[1], (CODE_UNDO_MAX - 1) * sizeof(code_undo_t));
        t->undo_n--;
    }
    code_undo_t *o = &t->undo[t->undo_n++];
    o->is_insert = is_insert; o->pos = pos; o->len = n;
    o->data = malloc(n);
    if (o->data) memcpy(o->data, data, n);
    undo_clear(t->redo, &t->redo_n);
}
static void do_undo(code_tab_t *t)
{
    if (!t->undo || !t->redo || t->undo_n == 0) return;
    code_undo_t o = t->undo[--t->undo_n];
    t->undo[t->undo_n].data = NULL;
    if (o.is_insert) tab_delete(t, o.pos, o.len);
    else { tab_insert(t, o.pos, o.data, o.len); t->cursor = o.pos; t->anchor = -1; }
    t->redo[t->redo_n++] = o;
    t->modified = true;
}
static void do_redo(code_tab_t *t)
{
    if (!t->undo || !t->redo || t->redo_n == 0) return;
    code_undo_t o = t->redo[--t->redo_n];
    t->redo[t->redo_n].data = NULL;
    if (o.is_insert) tab_insert(t, o.pos, o.data, o.len);
    else { tab_delete(t, o.pos, o.len); t->cursor = o.pos; t->anchor = -1; }
    t->undo[t->undo_n++] = o;
    t->modified = true;
}
static void tab_free(code_tab_t *t)
{
    if (t->undo) undo_clear(t->undo, &t->undo_n);
    if (t->redo) undo_clear(t->redo, &t->redo_n);
    free(t->undo);
    free(t->redo);
    free(t->text);
    t->text = NULL;
    t->undo = NULL;
    t->redo = NULL;
}

/* ------------------------------------------------------------------ */
/* Cursor / line helpers (operate on a tab)                            */
/* ------------------------------------------------------------------ */
static int line_start_of(const code_tab_t *t, int off)
{
    while (off > 0 && t->text[off - 1] != '\n') off--;
    return off;
}
static int line_end_of(const code_tab_t *t, int off)
{
    while (off < t->len && t->text[off] != '\n') off++;
    return off;
}
static int total_lines(const code_tab_t *t)
{
    int n = 1;
    for (int i = 0; i < t->len; i++) if (t->text[i] == '\n') n++;
    return n;
}
static void offset_to_lc(const code_tab_t *t, int off, int *line, int *col)
{
    int l = 0, c = 0;
    for (int i = 0; i < off && i < t->len; i++) {
        if (t->text[i] == '\n') { l++; c = 0; } else c++;
    }
    *line = l; *col = c;
}
static int line_offset_of(const code_tab_t *t, int line)
{
    int lo = 0, l = 0;
    while (l < line && lo < t->len) {
        if (t->text[lo] == '\n') l++;
        lo++;
    }
    return lo;
}
static int sel_start(const code_tab_t *t)
{
    if (t->anchor < 0) return t->cursor;
    return t->anchor < t->cursor ? t->anchor : t->cursor;
}
static int sel_end(const code_tab_t *t)
{
    if (t->anchor < 0) return t->cursor;
    return t->anchor < t->cursor ? t->cursor : t->anchor;
}
static void clear_sel(code_tab_t *t) { t->anchor = -1; }

/* ------------------------------------------------------------------ */
/* Clipboard                                                           */
/* ------------------------------------------------------------------ */
static void clip_copy(code_tab_t *t)
{
    if (t->anchor < 0) return;
    int a = sel_start(t), b = sel_end(t);
    int n = b - a;
    if (n > CODE_CLIP_MAX) n = CODE_CLIP_MAX;
    memcpy(g_clip, t->text + a, n);
    g_clip_len = n;
}
static void clip_cut(code_tab_t *t)
{
    if (t->anchor < 0) return;
    clip_copy(t);
    int a = sel_start(t), n = sel_end(t) - a;
    push_undo(t, false, a, t->text + a, n);
    tab_delete(t, a, n);
    t->cursor = a; clear_sel(t); t->modified = true;
}
static void clip_paste(code_tab_t *t)
{
    if (g_clip_len <= 0) return;
    if (t->anchor >= 0) {
        int a = sel_start(t), n = sel_end(t) - a;
        push_undo(t, false, a, t->text + a, n);
        tab_delete(t, a, n);
        t->cursor = a; clear_sel(t);
    }
    push_undo(t, true, t->cursor, g_clip, g_clip_len);
    tab_insert(t, t->cursor, g_clip, g_clip_len);
    t->modified = true;
}

/* ------------------------------------------------------------------ */
/* Tab load / save                                                     */
/* ------------------------------------------------------------------ */
static void load_into_tab(code_tab_t *t, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    if (sz > CODE_TEXT_MAX) sz = CODE_TEXT_MAX;
    tab_ensure(t, (int)sz);
    int got = (int)fread(t->text, 1, sz, f);
    fclose(f);
    t->len = got;
    t->text[t->len] = 0;
    t->cursor = 0; t->anchor = -1;
    t->scroll_x = 0; t->scroll_y = 0;
    t->modified = false;
    t->in_block = false;
    strncpy(t->path, path, sizeof(t->path) - 1);
    t->path[sizeof(t->path) - 1] = 0;
    undo_clear(t->undo, &t->undo_n);
    undo_clear(t->redo, &t->redo_n);
    code_lang_for(base_name(path), &t->lang);
}
static void save_tab(code_tab_t *t)
{
    if (!t->path[0]) return;
    FILE *f = fopen(t->path, "wb");
    if (!f) { return; }
    fwrite(t->text, 1, t->len, f);
    fclose(f);
    t->modified = false;
}

/* ------------------------------------------------------------------ */
/* Explorer tree                                                       */
/* ------------------------------------------------------------------ */
static void node_free(code_node_t *n)
{
    if (!n) return;
    for (int i = 0; i < n->child_count; i++) node_free(&n->children[i]);
    free(n->children);
    n->children = NULL;
    n->child_count = 0;
}
static int node_cmp(const void *a, const void *b)
{
    const code_node_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}
static void node_load(code_node_t *n)
{
    if (n->loaded || !n->is_dir) return;
    n->loaded = true;
    n->child_count = 0;
    DIR *d = opendir(n->path);
    if (!d) return;
    code_node_t *arr = NULL;
    int cap = 0, cnt = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (cnt >= cap) {
            cap = cap ? cap * 2 : 16;
            code_node_t *na = realloc(arr, cap * sizeof(code_node_t));
            if (!na) break;
            arr = na;
        }
        code_node_t *c = &arr[cnt++];
        memset(c, 0, sizeof(*c));
        strncpy(c->name, e->d_name, 63); c->name[63] = 0;
        int pk = snprintf(c->path, sizeof(c->path), "%s/", n->path);
        if (pk < (int)sizeof(c->path)) {
            strncpy(c->path + pk, e->d_name, sizeof(c->path) - pk - 1);
            c->path[sizeof(c->path) - 1] = 0;
        }
        struct stat st;
        if (stat(c->path, &st) == 0) {
            c->is_dir = S_ISDIR(st.st_mode);
            c->size = c->is_dir ? 0 : (uint32_t)st.st_size;
        } else { c->is_dir = false; c->size = 0; }
        c->parent = n;
        c->children = NULL; c->child_count = 0; c->loaded = false;
        c->expanded = false;
    }
    closedir(d);
    qsort(arr, cnt, sizeof(code_node_t), node_cmp);
    n->children = arr;
    n->child_count = cnt;
}
static void expl_add(code_state_t *s, code_node_t *n)
{
    if (s->vis_n >= s->vis_cap) {
        s->vis_cap = s->vis_cap ? s->vis_cap * 2 : 128;
        code_node_t **nv = realloc(s->vis, s->vis_cap * sizeof(code_node_t *));
        if (!nv) return;
        s->vis = nv;
    }
    s->vis[s->vis_n++] = n;
    if (n->is_dir && n->expanded) {
        node_load(n);
        for (int i = 0; i < n->child_count; i++)
            expl_add(s, &n->children[i]);
    }
}
static void explorer_build(code_state_t *s)
{
    s->vis_n = 0;
    if (s->root) expl_add(s, s->root);
    if (s->expl_sel >= s->vis_n) s->expl_sel = s->vis_n - 1;
    if (s->expl_sel < 0) s->expl_sel = 0;
}

/* ------------------------------------------------------------------ */
/* FS management (create / rename / delete)                            */
/* ------------------------------------------------------------------ */
static bool fs_create_file(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fclose(f);
    return true;
}
static bool fs_create_dir(const char *path)
{
    return mkdir(path, 0755) == 0;
}
static bool fs_rename(const char *oldp, const char *newp)
{
    return rename(oldp, newp) == 0;
}
static bool fs_delete(const char *path, bool is_dir)
{
    if (is_dir) return rmdir(path) == 0;
    return unlink(path) == 0;
}

/* ------------------------------------------------------------------ */
/* Syntax highlighting                                                 */
/* ------------------------------------------------------------------ */
static const char *KW_C[] = {
    "auto","break","case","char","const","continue","default","do","double",
    "else","enum","extern","float","for","goto","if","inline","int","long",
    "register","return","short","signed","sizeof","static","struct","switch",
    "typedef","union","unsigned","void","volatile","while","NULL",NULL
};
static const char *KW_CPP[] = {
    "class","public","private","protected","virtual","template","namespace",
    "using","new","delete","this","true","false","bool","catch","throw","try",
    "operator","friend","constexpr","override","nullptr",NULL
};
static const char *KW_PY[] = {
    "def","class","return","if","elif","else","for","while","import","from",
    "as","with","try","except","finally","raise","lambda","and","or","not",
    "in","is","None","True","False","pass","break","continue","global","yield",
    "async","await","print",NULL
};
static const char *KW_RUST[] = {
    "fn","let","mut","pub","struct","enum","impl","trait","use","mod","match",
    "if","else","for","while","loop","return","self","Self","crate","move",
    "ref","true","false","Some","None","Ok","Err",NULL
};
static const char *KW_JS[] = {
    "function","var","let","const","return","if","else","for","while","do",
    "switch","case","break","continue","new","class","extends","this","typeof",
    "instanceof","true","false","null","undefined","import","export","async",
    "await",NULL
};
static const char *KW_SH[] = {
    "if","then","else","elif","fi","for","in","do","done","while","case","esac",
    "function","return","export","local","echo","cd","exit",NULL
};
static const char **kws_for(int lang)
{
    switch (lang) {
        case LANG_C:   return KW_C;
        case LANG_CPP: return KW_CPP;
        case LANG_PY:  return KW_PY;
        case LANG_RUST:return KW_RUST;
        case LANG_JS:  return KW_JS;
        case LANG_SH:  return KW_SH;
        default:       return NULL;
    }
}
static bool is_ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
static bool is_num_start(char c) { return (c >= '0' && c <= '9'); }

enum { T_NONE, T_KW, T_STR, T_NUM, T_COM, T_PRE, T_PUN };
typedef struct { int start; int end; int type; } hl_tok_t;

static int hl_tokenize(const code_tab_t *t, int a, int b,
                       hl_tok_t *out, int maxout, bool *in_block)
{
    int n = 0;
    int i = a;
    const char *tx = t->text;
    bool block = (t->lang == LANG_C || t->lang == LANG_CPP || t->lang == LANG_RUST)
                 ? *in_block : false;
    while (i < b && n < maxout) {
        char c = tx[i];
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (block) {
            int j = i;
            while (j < b - 1 && !(tx[j] == '*' && tx[j + 1] == '/')) j++;
            int end = (j < b - 1) ? j + 2 : b;
            if (j < b - 1) { block = false; *in_block = false; }
            else *in_block = true;
            if (n < maxout) out[n++] = (hl_tok_t){i, end, T_COM};
            i = end; continue;
        }
        if (t->lang != LANG_NONE && c == '/' && i + 1 < b && tx[i + 1] == '/') {
            if (n < maxout) out[n++] = (hl_tok_t){i, b, T_COM};
            break;
        }
        if (t->lang != LANG_NONE && c == '/' && i + 1 < b && tx[i + 1] == '*') {
            int j = i + 2;
            while (j < b - 1 && !(tx[j] == '*' && tx[j + 1] == '/')) j++;
            if (j < b - 1) {
                if (n < maxout) out[n++] = (hl_tok_t){i, j + 2, T_COM};
                i = j + 2; continue;
            } else {
                if (n < maxout) out[n++] = (hl_tok_t){i, b, T_COM};
                *in_block = true; break;
            }
        }
        if (c == '"' || c == '\'') {
            char q = c;
            int j = i + 1;
            while (j < b && tx[j] != q) { if (tx[j] == '\\') j++; j++; }
            int end = (j < b) ? j + 1 : b;
            if (n < maxout) out[n++] = (hl_tok_t){i, end, T_STR};
            i = end; continue;
        }
        if ((t->lang == LANG_C || t->lang == LANG_CPP) && c == '#') {
            if (n < maxout) out[n++] = (hl_tok_t){i, b, T_PRE};
            break;
        }
        if (is_num_start(c) || (c == '.' && i + 1 < b && is_num_start(tx[i+1]))) {
            int j = i;
            while (j < b && ((tx[j] >= '0' && tx[j] <= '9') || tx[j] == '.' ||
                             tx[j] == 'x' || tx[j] == 'X' ||
                             (tx[j] >= 'a' && tx[j] <= 'f') ||
                             (tx[j] >= 'A' && tx[j] <= 'F') || tx[j] == '_')) j++;
            if (n < maxout) out[n++] = (hl_tok_t){i, j, T_NUM};
            i = j; continue;
        }
        if (is_ident_char(c)) {
            int j = i;
            while (j < b && is_ident_char(tx[j])) j++;
            bool kw = false;
            const char **k = kws_for(t->lang);
            if (k) {
                char tmp[64]; int w = j - i;
                if (w >= 64) w = 63;
                memcpy(tmp, tx + i, w); tmp[w] = 0;
                for (int z = 0; k[z]; z++) if (!strcmp(tmp, k[z])) { kw = true; break; }
            }
            if (n < maxout) out[n++] = (hl_tok_t){i, j, kw ? T_KW : T_NONE};
            i = j; continue;
        }
        if (n < maxout) out[n++] = (hl_tok_t){i, i + 1, T_PUN};
        i++;
    }
    return n;
}
static uint16_t tok_color(int type)
{
    switch (type) {
        case T_KW:  return HL_KW;
        case T_STR: return HL_STR;
        case T_NUM: return HL_NUM;
        case T_COM: return HL_COM;
        case T_PRE: return HL_PRE;
        default:    return HL_TEXT;
    }
}

/* ------------------------------------------------------------------ */
/* Rendering: tabs + explorer                                          */
/* ------------------------------------------------------------------ */
static code_tab_t *act_tab(code_state_t *s)
{
    if (s->tab_n <= 0) return NULL;
    if (s->tab_active < 0 || s->tab_active >= s->tab_n) s->tab_active = 0;
    return &s->tabs[s->tab_active];
}
static int code_tab_layout(code_state_t *s, int x0, int *x, int *w, int *cx)
{
    int xx = x0 + 2;
    for (int i = 0; i < s->tab_n; i++) {
        const char *nm = base_name(s->tabs[i].path);
        if (!*nm) nm = "untitled";
        int tw = ui_text_width(nm);
        w[i] = tw + 24;
        x[i] = xx;
        cx[i] = xx + w[i] - 12;
        xx += w[i] + 2;
    }
    return s->tab_n;
}
static void draw_tabs(ili9488_t *lcd, const ui_rect_t *c, code_state_t *s)
{
    ui_rect_t bar = { c->x, c->y, c->w, CODE_TAB_H };
    ui_draw_fill_rect(lcd, &bar, UI_C_PANEL);
    int tx[CODE_MAX_TABS], tw[CODE_MAX_TABS], tcx[CODE_MAX_TABS];
    code_tab_layout(s, c->x, tx, tw, tcx);
    for (int i = 0; i < s->tab_n; i++) {
        bool on = (i == s->tab_active);
        ui_rect_t r = { (uint16_t)tx[i], c->y, (uint16_t)tw[i], CODE_TAB_H };
        ui_draw_fill_rect(lcd, &r, on ? UI_C_WIN_BG : UI_C_PANEL);
        const char *nm = base_name(s->tabs[i].path);
        if (!*nm) nm = "untitled";
        ui_draw_text(lcd, (uint16_t)(tx[i] + 4), (uint16_t)(c->y + 2), nm,
                     on ? UI_C_TEXT : UI_C_PANEL_TEXT,
                     on ? UI_C_WIN_BG : UI_C_PANEL);
        if (s->tabs[i].modified)
            ui_draw_text(lcd, (uint16_t)(tcx[i] - 6), (uint16_t)(c->y + 2), "*",
                         UI_C_ACCENT, on ? UI_C_WIN_BG : UI_C_PANEL);
        ui_draw_text(lcd, (uint16_t)tcx[i], (uint16_t)(c->y + 2), "x",
                     UI_C_TEXT_DIM, on ? UI_C_WIN_BG : UI_C_PANEL);
    }
}
static int node_depth(const code_node_t *n)
{
    int d = 0;
    while (n->parent) { d++; n = n->parent; }
    return d;
}
static void draw_explorer(ili9488_t *lcd, const ui_rect_t *c, code_state_t *s)
{
    ui_draw_fill_rect(lcd, c, UI_C_WIN_BG);
    ui_draw_text(lcd, (uint16_t)(c->x + 4), (uint16_t)(c->y + 2), "EXPLORER",
                 UI_C_TEXT_DIM, UI_C_WIN_BG);
    int rows = (c->h - CODE_LH) / CODE_LH;
    if (rows < 1) rows = 1;
    s->expl_rows = rows;
    /* keep the selected row visible */
    if (s->expl_sel < s->expl_scroll) s->expl_scroll = s->expl_sel;
    if (s->expl_sel >= s->expl_scroll + rows) s->expl_scroll = s->expl_sel - rows + 1;
    if (s->expl_scroll < 0) s->expl_scroll = 0;
    if (s->expl_scroll > s->vis_n - rows) s->expl_scroll = s->vis_n - rows;
    if (s->expl_scroll < 0) s->expl_scroll = 0;

    for (int row = 0; row < rows; row++) {
        int idx = s->expl_scroll + row;
        if (idx >= s->vis_n) break;
        code_node_t *n = s->vis[idx];
        int y = c->y + CODE_LH + row * CODE_LH;
        if (idx == s->expl_sel) {
            ui_rect_t hl = { c->x, (uint16_t)y, c->w, CODE_LH };
            ui_draw_fill_rect(lcd, &hl, UI_C_ACCENT);
        }
        int d = node_depth(n);
        int ix = c->x + 4 + d * 12;
        const char *mark = n->is_dir ? (n->expanded ? "[-] " : "[+] ") : "    ";
        /* truncate the label so it never overflows into the editor pane */
        int avail = (int)c->w - (ix - c->x) - 3;
        if (avail < CHAR_W) avail = CHAR_W;
        int maxc = avail / CHAR_W;
        char label[80];
        snprintf(label, sizeof(label), "%s%s", mark, n->name);
        int L = strlen(label);
        if (L > maxc && maxc >= 1) {
            if (maxc >= 2) { label[maxc - 2] = '>'; label[maxc - 1] = 0; }
            else { label[0] = '>'; label[1] = 0; }
        }
        ui_draw_text(lcd, (uint16_t)ix, (uint16_t)y, label,
                     n->is_dir ? UI_C_TEXT : UI_C_TEXT,
                     idx == s->expl_sel ? UI_C_ACCENT : UI_C_WIN_BG);
    }
    /* scroll hints */
    if (s->expl_scroll > 0)
        ui_draw_text(lcd, (uint16_t)(c->x + c->w - 10), (uint16_t)(c->y + 2),
                     "+", UI_C_TEXT_DIM, UI_C_WIN_BG);
    if (s->expl_scroll + rows < s->vis_n)
        ui_draw_text(lcd, (uint16_t)(c->x + c->w - 10),
                     (uint16_t)(c->y + c->h - CODE_LH), "^",
                     UI_C_TEXT_DIM, UI_C_WIN_BG);
}

/* ------------------------------------------------------------------ */
/* Rendering: editor + output + dispatch                               */
/* ------------------------------------------------------------------ */
static void draw_editor(ili9488_t *lcd, const ui_rect_t *c, code_state_t *s)
{
    code_tab_t *t = act_tab(s);
    if (!t) {
        ui_draw_text(lcd, (uint16_t)(c->x + CODE_GUTTER + 4), (uint16_t)(c->y + 4),
                     "No file open. Use the explorer (left) to open one.",
                     UI_C_TEXT_DIM, UI_C_WIN_BG);
        return;
    }
    int rows = c->h / CODE_LH;
    int total = total_lines(t);
    int vis_cols = (c->w - CODE_GUTTER) / CHAR_W;
    if (vis_cols < 1) vis_cols = 1;

    int cl = 0, ccol = 0;
    offset_to_lc(t, t->cursor, &cl, &ccol);
    if (ccol < t->scroll_x) t->scroll_x = ccol;
    if (ccol >= t->scroll_x + vis_cols) t->scroll_x = ccol - vis_cols + 1;
    if (t->scroll_x < 0) t->scroll_x = 0;
    if (cl < t->scroll_y) t->scroll_y = cl;
    if (cl >= t->scroll_y + rows) t->scroll_y = cl - rows + 1;
    if (t->scroll_y < 0) t->scroll_y = 0;
    if (t->scroll_y > total - rows) t->scroll_y = total - rows;
    if (t->scroll_y < 0) t->scroll_y = 0;

    if (t->len == 0) {
        ui_draw_text(lcd, (uint16_t)(c->x + CODE_GUTTER + 4), (uint16_t)(c->y + 2),
                     "untitled - type, Ctrl+S save, Ctrl+O open",
                     UI_C_TEXT_DIM, UI_C_WIN_BG);
    }

    bool blk = false;
    for (int ln = 0; ln < t->scroll_y; ln++) {
        int a = line_offset_of(t, ln);
        int b = (ln + 1 < total) ? line_offset_of(t, ln + 1) : t->len;
        hl_tok_t tk[48];
        hl_tokenize(t, a, b, tk, 48, &blk);
    }

    int sel_a = sel_start(t), sel_b = sel_end(t);
    bool has_sel = t->anchor >= 0 && sel_b > sel_a;

    for (int row = 0; row < rows; row++) {
        int line = t->scroll_y + row;
        if (line >= total) break;
        int a = line_offset_of(t, line);
        int b = (line + 1 < total) ? line_offset_of(t, line + 1) : t->len;
        if (b > a && t->text[b - 1] == '\n') b--;

        char ln[16];
        snprintf(ln, sizeof ln, "%4d", line + 1);
        ui_draw_text(lcd, (uint16_t)(c->x + 2),
                     (uint16_t)(c->y + row * CODE_LH), ln,
                     UI_C_TEXT_DIM, UI_C_WIN_BG);

        hl_tok_t toks[64];
        int nt = hl_tokenize(t, a, b, toks, 64, &blk);

        int base_y = c->y + row * CODE_LH;
        for (int col = 0; a + col < b; col++) {
            int off = a + col;
            if (col < t->scroll_x) continue;
            int sx = c->x + CODE_GUTTER + (col - t->scroll_x) * CHAR_W;
            if (sx >= c->x + c->w) break;
            char ch = t->text[off];
            if (ch == '\t') ch = ' ';
            uint16_t fg = HL_TEXT, bg = UI_C_WIN_BG;
            for (int ti = 0; ti < nt; ti++) {
                if (off >= toks[ti].start && off < toks[ti].end) {
                    fg = tok_color(toks[ti].type); break;
                }
            }
            bool in_sel = has_sel && off >= sel_a && off < sel_b;
            if (in_sel) { bg = UI_C_ACCENT; fg = UI_C_WIN_BG; }
            if (off == t->cursor) { bg = UI_C_ACCENT; fg = UI_C_TEXT; }
            char tmp[2] = { ch, 0 };
            ui_draw_text(lcd, (uint16_t)sx, (uint16_t)base_y, tmp, fg, bg);
        }
    }
}

static void draw_output(ili9488_t *lcd, const ui_rect_t *c, code_state_t *s)
{
    ui_rect_t hdr = { c->x, c->y, c->w, CODE_LH };
    ui_draw_fill_rect(lcd, &hdr, UI_C_PANEL);
    ui_draw_text(lcd, (uint16_t)(c->x + 4), (uint16_t)(c->y + 1),
                 "OUTPUT  (c clear  PgUp/PgDn)", UI_C_PANEL_TEXT, UI_C_PANEL);
    ui_rect_t body = { c->x, (uint16_t)(c->y + CODE_LH), c->w,
                       (uint16_t)(c->h - CODE_LH) };
    ui_draw_fill_rect(lcd, &body, UI_C_WIN_BG);

    int body_rows = body.h / CODE_LH;
    int total = 1;
    for (int i = 0; i < s->out.len; i++) if (s->out.buf[i] == '\n') total++;
    int first = total - body_rows - s->out.scroll;
    if (first < 0) first = 0;

    int li = 0, row = 0;
    int i = 0;
    while (i < s->out.len && row < body_rows) {
        int j = i;
        while (j < s->out.len && s->out.buf[j] != '\n') j++;
        if (li >= first) {
            int maxw = (body.w - 8) / CHAR_W;
            if (maxw < 1) maxw = 1;
            int ll = j - i;
            if (ll > maxw) ll = maxw;
            char line[256];
            memcpy(line, s->out.buf + i, ll); line[ll] = 0;
            ui_draw_text(lcd, (uint16_t)(body.x + 4),
                         (uint16_t)(body.y + row * CODE_LH), line,
                         UI_C_TEXT, UI_C_WIN_BG);
            row++;
        }
        li++;
        i = (j < s->out.len) ? j + 1 : j;
    }
}

static void draw_prompt(ili9488_t *lcd, const ui_rect_t *c, code_state_t *s)
{
    int y = c->y + c->h - CODE_LH;
    ui_rect_t bar = { c->x, (uint16_t)y, c->w, CODE_LH };
    ui_draw_fill_rect(lcd, &bar, UI_C_PANEL);
    char line[160];
    snprintf(line, sizeof(line), "%s %s", s->prompt_label, s->input);
    ui_draw_text(lcd, (uint16_t)(c->x + 4), (uint16_t)(y + 1), line,
                 UI_C_PANEL_TEXT, UI_C_PANEL);
    int cw = ui_text_width(line);
    ui_draw_vline(lcd, (uint16_t)(c->x + 4 + cw), (uint16_t)y + 2,
                 (uint16_t)(CODE_LH - 4), UI_C_ACCENT);
}

static void code_render(arpile_app_ctx_t *ctx, ui_win_t *win)
{
    code_state_t *s = ctx->user;
    ili9488_t *lcd = arpile_ui_get_lcd();
    ui_rect_t c = win_client_rect(win);
    if (!s) {
        ui_draw_fill_rect(lcd, &c, 0xF800);
        ui_draw_text(lcd, c.x + 4, c.y + 4, "CODE: ctx->user NULL (init not run)",
                     UI_C_TEXT_LIGHT, 0xF800);
        return;
    }
    ui_draw_fill_rect(lcd, &c, UI_C_WIN_BG);

    ui_rect_t tabs = { c.x, c.y, c.w, CODE_TAB_H };
    int y1 = c.y + CODE_TAB_H;
    int out_h = s->out_open ? CODE_OUT_H : 0;
    int edit_h = c.h - CODE_TAB_H - out_h;
    if (edit_h < CODE_LH) edit_h = CODE_LH;
    ui_rect_t expl = { c.x, (uint16_t)y1, CODE_EXPL_W, (uint16_t)edit_h };
    ui_rect_t ed = { (uint16_t)(c.x + CODE_EXPL_W), y1,
                     (uint16_t)(c.w - CODE_EXPL_W), (uint16_t)edit_h };
    ui_rect_t outr = { c.x, (uint16_t)(y1 + edit_h), c.w, (uint16_t)out_h };

    draw_tabs(lcd, &tabs, s);

    /* vertical separator between explorer and editor */
    int sep_x = c.x + CODE_EXPL_W;
    ui_draw_vline(lcd, (uint16_t)sep_x, (uint16_t)y1, (uint16_t)edit_h, UI_C_BORDER);

    draw_explorer(lcd, &expl, s);
    draw_editor(lcd, &ed, s);

    /* horizontal separator above output panel */
    if (out_h) {
        int sep_y = y1 + edit_h;
        ui_draw_hline(lcd, (uint16_t)c.x, (uint16_t)sep_y, (uint16_t)c.w, UI_C_BORDER);
        draw_output(lcd, &outr, s);
    }
    if (s->prompt != PMPT_NONE) draw_prompt(lcd, &c, s);
}

/* ------------------------------------------------------------------ */
/* Ctrl+P command runner (reuses terminal FS command semantics)         */
/* ------------------------------------------------------------------ */
static void code_resolve(code_state_t *s, const char *arg, char *dst, int dsz)
{
    if (arg && arg[0] == '/') {
        strncpy(dst, arg, dsz - 1); dst[dsz - 1] = 0; return;
    }
    int k = snprintf(dst, dsz, "%s", s->exec_cwd);
    if (k < dsz && k > 0) {
        if (dst[k - 1] != '/') { dst[k] = '/'; k++; dst[k] = 0; }
        strncpy(dst + k, arg ? arg : "", dsz - k - 1);
        dst[dsz - 1] = 0;
    }
}
static void code_exec(code_state_t *s, const char *cmd)
{
    char copy[128];
    strncpy(copy, cmd, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = 0;
    out_appendln(s, (char *)cmd);
    char *argv[8]; int argc = 0;
    char *p = copy;
    while (*p && argc < 8) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    if (argc == 0) return;
    const char *c0 = argv[0];
    if (!strcmp(c0, "clear")) { s->out.len = 0; s->out.scroll = 0; return; }
    if (!strcmp(c0, "echo")) {
        char line[192];
        line[0] = 0;
        for (int i = 1; i < argc; i++) {
            strcat(line, argv[i]);
            if (i < argc - 1) strcat(line, " ");
        }
        out_appendln(s, line);
    } else if (!strcmp(c0, "pwd")) {
        out_appendln(s, s->exec_cwd);
    } else if (!strcmp(c0, "ls") || !strcmp(c0, "dir")) {
        DIR *d = opendir(s->exec_cwd);
        if (!d) { out_appendln(s, "Error: cannot open directory"); return; }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char path[256]; code_resolve(s, e->d_name, path, sizeof(path));
            struct stat st; bool isdir = false;
            if (stat(path, &st) == 0) isdir = S_ISDIR(st.st_mode);
            char line[300];
            snprintf(line, sizeof(line), "  %s%s", e->d_name, isdir ? "/" : "");
            out_appendln(s, line);
        }
        closedir(d);
    } else if (!strcmp(c0, "cd")) {
        char np[256];
        if (!argv[1] || !*argv[1]) { strcpy(np, "/"); }
        else code_resolve(s, argv[1], np, sizeof(np));
        DIR *d = opendir(np);
        if (d) { closedir(d); strncpy(s->exec_cwd, np, sizeof(s->exec_cwd) - 1);
                 s->exec_cwd[sizeof(s->exec_cwd) - 1] = 0; }
        else out_appendln(s, "Error: directory not found");
    } else if (!strcmp(c0, "cat")) {
        if (!argv[1]) { out_appendln(s, "Usage: cat <file>"); return; }
        char path[256]; code_resolve(s, argv[1], path, sizeof(path));
        FILE *f = fopen(path, "r");
        if (!f) { out_appendln(s, "Error: file not found"); return; }
        char line[128];
        while (fgets(line, sizeof(line), f)) out_append(s, line);
        fclose(f);
    } else if (!strcmp(c0, "mkdir")) {
        if (!argv[1]) { out_appendln(s, "Usage: mkdir <dir>"); return; }
        char path[256]; code_resolve(s, argv[1], path, sizeof(path));
        if (mkdir(path, 0755) != 0) out_appendln(s, "Error: cannot create directory");
    } else if (!strcmp(c0, "rm")) {
        if (!argv[1]) { out_appendln(s, "Usage: rm <file>"); return; }
        char path[256]; code_resolve(s, argv[1], path, sizeof(path));
        struct stat st; bool isdir = false;
        if (stat(path, &st) == 0) isdir = S_ISDIR(st.st_mode);
        if (fs_delete(path, isdir)) out_appendln(s, "removed");
        else out_appendln(s, "Error: cannot remove");
    } else if (!strcmp(c0, "cp")) {
        if (!argv[1] || !argv[2]) { out_appendln(s, "Usage: cp <src> <dst>"); return; }
        char sp[256], dp[256];
        code_resolve(s, argv[1], sp, sizeof(sp));
        code_resolve(s, argv[2], dp, sizeof(dp));
        FILE *a = fopen(sp, "r"); if (!a) { out_appendln(s, "Error: src not found"); return; }
        FILE *b = fopen(dp, "w"); if (!b) { fclose(a); out_appendln(s, "Error: dst"); return; }
        char buf[128]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), a)) > 0) fwrite(buf, 1, n, b);
        fclose(a); fclose(b);
    } else if (!strcmp(c0, "mv")) {
        if (!argv[1] || !argv[2]) { out_appendln(s, "Usage: mv <src> <dst>"); return; }
        char sp[256], dp[256];
        code_resolve(s, argv[1], sp, sizeof(sp));
        code_resolve(s, argv[2], dp, sizeof(dp));
        if (rename(sp, dp) != 0) out_appendln(s, "Error: cannot move");
    } else if (!strcmp(c0, "help")) {
        out_appendln(s, "Commands: ls cd pwd cat mkdir rm cp mv echo clear");
    } else {
        char msg[80];
        snprintf(msg, sizeof(msg), "%s: not available on device", c0);
        out_appendln(s, msg);
    }
}

/* ------------------------------------------------------------------ */
/* Tab management + find                                               */
/* ------------------------------------------------------------------ */
static void dir_of(const char *path, char *dst, int dsz)
{
    const char *sl = strrchr(path, '/');
    if (sl) {
        int n = sl - path;
        if (n >= dsz) n = dsz - 1;
        memcpy(dst, path, n); dst[n] = 0;
    } else strcpy(dst, "/");
}
static void open_file_in_tab(code_state_t *s, const char *path)
{
    for (int i = 0; i < s->tab_n; i++)
        if (!strcmp(s->tabs[i].path, path)) { s->tab_active = i; return; }
    int idx = s->tab_n;
    if (idx >= CODE_MAX_TABS) idx = CODE_MAX_TABS - 1;
    code_tab_t *t = &s->tabs[idx];
    memset(t, 0, sizeof *t);
    t->text = malloc(1);
    if (t->text) t->text[0] = 0;
    t->undo = calloc(CODE_UNDO_MAX, sizeof(code_undo_t));
    t->redo = calloc(CODE_UNDO_MAX, sizeof(code_undo_t));
    t->cap = 1; t->len = 0; t->cursor = 0; t->anchor = -1;
    load_into_tab(t, path);
    if (idx == s->tab_n) s->tab_n++;
    s->tab_active = idx;
    dir_of(path, s->exec_cwd, sizeof(s->exec_cwd));
    explorer_build(s);
}
static void close_tab(code_state_t *s, int idx)
{
    if (idx < 0 || idx >= s->tab_n) return;
    tab_free(&s->tabs[idx]);
    for (int i = idx; i < s->tab_n - 1; i++) s->tabs[i] = s->tabs[i + 1];
    s->tab_n--;
    if (s->tab_active >= s->tab_n) s->tab_active = s->tab_n - 1;
    if (s->tab_active < 0) s->tab_active = 0;
}
static void save_all(code_state_t *s)
{
    for (int i = 0; i < s->tab_n; i++)
        if (s->tabs[i].modified) save_tab(&s->tabs[i]);
}
static void find_in_tab(code_tab_t *t, const char *needle)
{
    if (!needle || !*needle) return;
    char *p = (char *)strcasestr(t->text, needle);
    if (!p) return;
    t->cursor = (int)(p - t->text);
    t->anchor = -1;
    int l = 0, c = 0; offset_to_lc(t, t->cursor, &l, &c);
    t->scroll_y = l - 2;
    if (t->scroll_y < 0) t->scroll_y = 0;
}

/* ------------------------------------------------------------------ */
/* Explorer keyboard                                                   */
/* ------------------------------------------------------------------ */
static void expl_toggle_expand(code_state_t *s, code_node_t *n)
{
    if (!n || !n->is_dir) return;
    n->expanded = !n->expanded;
    explorer_build(s);
}
static code_node_t *expl_selected(code_state_t *s)
{
    if (s->expl_sel < 0 || s->expl_sel >= s->vis_n) return NULL;
    return s->vis[s->expl_sel];
}
static void code_key_explorer(code_state_t *s, const arpile_input_event_t *ev)
{
    code_node_t *n = expl_selected(s);
    switch (ev->key.keycode) {
        case ARPILE_KEY_UP:
            if (--s->expl_sel < 0) s->expl_sel = 0;
            explorer_build(s); return;
        case ARPILE_KEY_DOWN:
            if (++s->expl_sel >= s->vis_n) s->expl_sel = s->vis_n - 1;
            explorer_build(s); return;
        case ARPILE_KEY_HOME: s->expl_sel = 0; explorer_build(s); return;
        case ARPILE_KEY_END: s->expl_sel = s->vis_n - 1; explorer_build(s); return;
        case ARPILE_KEY_LEFT:
            if (n && n->is_dir && n->expanded) { n->expanded = false; explorer_build(s); }
            else if (n && n->parent) {
                for (int i = 0; i < s->vis_n; i++)
                    if (s->vis[i] == n->parent) { s->expl_sel = i; break; }
                explorer_build(s);
            }
            return;
        case ARPILE_KEY_RIGHT:
            if (n && n->is_dir) {
                if (!n->expanded) { n->expanded = true; explorer_build(s); }
                else if (n->child_count > 0) {
                    for (int i = 0; i < s->vis_n; i++)
                        if (s->vis[i] == &n->children[0]) { s->expl_sel = i; break; }
                    explorer_build(s);
                }
            } else if (n) open_file_in_tab(s, n->path);
            return;
        case ARPILE_KEY_ENTER:
            if (n && n->is_dir) expl_toggle_expand(s, n);
            else if (n) open_file_in_tab(s, n->path);
            return;
        case 0x35: /* backtick toggles editor focus */
            s->pane = PANE_EDITOR; return;
        case 0x11: /* n */
            s->prompt = (ev->key.modifier & (ARPILE_MOD_LSHIFT | ARPILE_MOD_RSHIFT))
                        ? PMPT_NEWFOLDER : PMPT_NEWFILE;
            s->input_pos = 0; s->input[0] = 0;
            strncpy(s->prompt_label,
                    s->prompt == PMPT_NEWFOLDER ? "New folder:" : "New file:",
                    sizeof(s->prompt_label) - 1);
            return;
        case 0x15: /* r rename */
            if (n) {
                s->rename_node = n; s->prompt = PMPT_RENAME;
                strncpy(s->input, n->name, 127); s->input[127] = 0;
                s->input_pos = strlen(s->input);
                strncpy(s->prompt_label, "Rename:", sizeof(s->prompt_label) - 1);
            }
            return;
        case 0x07: /* d delete */
            if (n) {
                if (fs_delete(n->path, n->is_dir)) {
                    if (n->parent) n->parent->loaded = false;
                    s->expl_sel = 0;
                    explorer_build(s);
                } else strncpy(s->msg, "delete failed", sizeof(s->msg));
            }
            return;
        case 0x06: /* c clear output */
            s->out.len = 0; s->out.scroll = 0; return;
        default: break;
    }
}

static void new_untitled_tab(code_state_t *s)
{
    if (s->tab_n >= CODE_MAX_TABS) { s->tab_active = CODE_MAX_TABS - 1; return; }
    int idx = s->tab_n++;
    code_tab_t *t = &s->tabs[idx];
    memset(t, 0, sizeof *t);
    t->text = malloc(1);
    if (t->text) t->text[0] = 0;
    t->undo = calloc(CODE_UNDO_MAX, sizeof(code_undo_t));
    t->redo = calloc(CODE_UNDO_MAX, sizeof(code_undo_t));
    t->cap = 1; t->len = 0; t->cursor = 0; t->anchor = -1;
    t->path[0] = 0;
    s->tab_active = idx;
}

static void code_key_editor(code_state_t *s, code_tab_t *t, const arpile_input_event_t *ev)
{
    uint8_t m = ev->key.modifier;
    bool ctrl = (m & (ARPILE_MOD_LCTRL | ARPILE_MOD_RCTRL)) != 0;
    bool shift = (m & (ARPILE_MOD_LSHIFT | ARPILE_MOD_RSHIFT)) != 0;
    if (ctrl) {
        switch (ev->key.keycode) {
            case 0x19: clip_paste(t); return;
            case 0x06: clip_copy(t); return;
            case 0x1b: clip_cut(t); return;
            case 0x1d: do_undo(t); return;
            case 0x1c: do_redo(t); return;
            case 0x11: new_untitled_tab(s); return;
            case 0x1a: close_tab(s, s->tab_active); return;
            case 0x12: s->pane = PANE_EXPLORER; return;
            case 0x09: s->prompt = PMPT_FIND; s->input_pos = 0; s->input[0] = 0;
                      strncpy(s->prompt_label, "Find:", sizeof(s->prompt_label) - 1); return;
            case 0x04: t->anchor = 0; t->cursor = t->len; return;
            case 0x16: /* s */
                if (shift) save_all(s);
                else if (t->path[0]) save_tab(t);
                else { s->prompt = PMPT_SAVEAS; s->input_pos = 0; s->input[0] = 0;
                       strncpy(s->prompt_label, "Save as:", sizeof(s->prompt_label) - 1); }
                return;
            default: return;
        }
    }
    switch (ev->key.keycode) {
        case ARPILE_KEY_UP: {
            int l = 0, c = 0; offset_to_lc(t, t->cursor, &l, &c);
            if (l > 0) {
                int nl = line_offset_of(t, l - 1);
                int nlend = line_end_of(t, nl);
                int nc = nl + (c < (nlend - nl) ? c : (nlend - nl));
                if (!shift && t->anchor >= 0) clear_sel(t);
                if (shift && t->anchor < 0) t->anchor = t->cursor;
                t->cursor = nc;
            }
            return;
        }
        case ARPILE_KEY_DOWN: {
            int l = 0, c = 0; offset_to_lc(t, t->cursor, &l, &c);
            int tot = total_lines(t);
            if (l < tot - 1) {
                int nl = line_offset_of(t, l + 1);
                int nlend = line_end_of(t, nl);
                int nc = nl + (c < (nlend - nl) ? c : (nlend - nl));
                if (!shift && t->anchor >= 0) clear_sel(t);
                if (shift && t->anchor < 0) t->anchor = t->cursor;
                t->cursor = nc;
            }
            return;
        }
        case ARPILE_KEY_LEFT:
            if (!shift && t->anchor >= 0) { t->cursor = sel_start(t); clear_sel(t); return; }
            if (t->cursor > 0) {
                if (shift && t->anchor < 0) t->anchor = t->cursor;
                t->cursor--;
            }
            return;
        case ARPILE_KEY_RIGHT:
            if (!shift && t->anchor >= 0) { t->cursor = sel_end(t); clear_sel(t); return; }
            if (t->cursor < t->len) {
                if (shift && t->anchor < 0) t->anchor = t->cursor;
                t->cursor++;
            }
            return;
        case ARPILE_KEY_HOME:
            t->cursor = line_start_of(t, t->cursor);
            if (!shift) clear_sel(t); else if (t->anchor < 0) t->anchor = t->cursor;
            return;
        case ARPILE_KEY_END:
            t->cursor = line_end_of(t, t->cursor);
            if (!shift) clear_sel(t); else if (t->anchor < 0) t->anchor = t->cursor;
            return;
        case ARPILE_KEY_PGUP: t->scroll_y -= 1; return;
        case ARPILE_KEY_PGDN: t->scroll_y += 1; return;
        case ARPILE_KEY_ENTER: {
            if (t->anchor >= 0) {
                int a = sel_start(t), n = sel_end(t) - a;
                push_undo(t, false, a, t->text + a, n);
                tab_delete(t, a, n); t->cursor = a; clear_sel(t);
            }
            push_undo(t, true, t->cursor, "\n", 1);
            tab_insert(t, t->cursor, "\n", 1); t->modified = true; return;
        }
        case ARPILE_KEY_BACKSPACE: {
            if (t->anchor >= 0) {
                int a = sel_start(t), n = sel_end(t) - a;
                push_undo(t, false, a, t->text + a, n);
                tab_delete(t, a, n); t->cursor = a; clear_sel(t); t->modified = true;
            } else if (t->cursor > 0) {
                push_undo(t, false, t->cursor - 1, t->text + t->cursor - 1, 1);
                tab_delete(t, t->cursor - 1, 1); t->modified = true;
            }
            return;
        }
        case ARPILE_KEY_DELETE: {
            if (t->anchor >= 0) {
                int a = sel_start(t), n = sel_end(t) - a;
                push_undo(t, false, a, t->text + a, n);
                tab_delete(t, a, n); t->cursor = a; clear_sel(t); t->modified = true;
            } else if (t->cursor < t->len) {
                push_undo(t, false, t->cursor, t->text + t->cursor, 1);
                tab_delete(t, t->cursor, 1); t->modified = true;
            }
            return;
        }
        case ARPILE_KEY_TAB: {
            if (t->anchor >= 0) {
                int a = sel_start(t), n = sel_end(t) - a;
                push_undo(t, false, a, t->text + a, n);
                tab_delete(t, a, n); t->cursor = a; clear_sel(t);
            }
            const char *tab = "    ";
            push_undo(t, true, t->cursor, tab, 4);
            tab_insert(t, t->cursor, tab, 4); t->modified = true; return;
        }
        case ARPILE_KEY_ESCAPE: s->pane = PANE_EXPLORER; return;
        default: break;
    }
    if (ev->key.ascii >= 0x20 && ev->key.ascii != 0x7f) {
        char ch = ev->key.ascii;
        if (t->anchor >= 0) {
            int a = sel_start(t), n = sel_end(t) - a;
            push_undo(t, false, a, t->text + a, n);
            tab_delete(t, a, n); t->cursor = a; clear_sel(t);
        }
        push_undo(t, true, t->cursor, &ch, 1);
        tab_insert(t, t->cursor, &ch, 1); t->modified = true;
    }
}

static void code_key_output(code_state_t *s, const arpile_input_event_t *ev)
{
    switch (ev->key.keycode) {
        case ARPILE_KEY_PGUP: s->out.scroll += 1; return;
        case ARPILE_KEY_PGDN:
            s->out.scroll -= 1; if (s->out.scroll < 0) s->out.scroll = 0; return;
        case 0x06: s->out.len = 0; s->out.scroll = 0; return; /* c clear */
        default: break;
    }
}

static code_node_t *node_find(code_node_t *n, const char *path)
{
    if (!n) return NULL;
    if (!strcmp(n->path, path)) return n;
    node_load(n);
    for (int i = 0; i < n->child_count; i++) {
        code_node_t *r = node_find(&n->children[i], path);
        if (r) return r;
    }
    return NULL;
}
static code_node_t *get_target_dir(code_state_t *s)
{
    code_node_t *n = expl_selected(s);
    if (!n) return s->root;
    if (n->is_dir) return n;
    return n->parent ? n->parent : s->root;
}

static void code_input_key(code_state_t *s, const arpile_input_event_t *ev)
{
    switch (ev->key.keycode) {
        case ARPILE_KEY_ENTER: {
            int pmpt = s->prompt;
            char buf[128]; strncpy(buf, s->input, 127); buf[127] = 0;
            s->prompt = PMPT_NONE; s->input[0] = 0; s->input_pos = 0;
            if (pmpt == PMPT_CMD) {
                code_exec(s, buf);
                s->pane = PANE_OUTPUT;
            } else if (pmpt == PMPT_FIND) {
                if (act_tab(s)) find_in_tab(act_tab(s), buf);
            } else if (pmpt == PMPT_SAVEAS) {
                code_tab_t *t = act_tab(s);
                if (t && buf[0]) {
                    char fp[400];
                    if (buf[0] == '/') snprintf(fp, sizeof(fp), "%s", buf);
                    else snprintf(fp, sizeof(fp), "%s/%s", s->exec_cwd, buf);
                    strncpy(t->path, fp, sizeof(t->path) - 1);
                    t->path[sizeof(t->path) - 1] = 0;
                    save_tab(t);
                    code_node_t *dn = node_find(s->root, s->exec_cwd);
                    if (dn) dn->loaded = false;
                    explorer_build(s);
                }
            } else if (pmpt == PMPT_RENAME) {
                code_node_t *n = s->rename_node;
                if (n && buf[0]) {
                    char np[400];
                    char dir[256]; dir_of(n->path, dir, sizeof(dir));
                    snprintf(np, sizeof(np), "%s/%s", dir, buf);
                    if (fs_rename(n->path, np) == 0) {
                        strncpy(n->name, buf, 63); n->name[63] = 0;
                        strncpy(n->path, np, sizeof(n->path) - 1);
                        n->path[sizeof(n->path) - 1] = 0;
                        if (n->parent) n->parent->loaded = false;
                        explorer_build(s);
                    }
                }
                s->rename_node = NULL;
            } else if (pmpt == PMPT_NEWFILE || pmpt == PMPT_NEWFOLDER) {
                code_node_t *dn = get_target_dir(s);
                if (dn && buf[0]) {
                    char fp[400];
                    snprintf(fp, sizeof(fp), "%s/%s", dn->path, buf);
                    bool ok = (pmpt == PMPT_NEWFILE) ? fs_create_file(fp)
                                                     : fs_create_dir(fp);
                    if (ok) { dn->loaded = false; explorer_build(s); }
                }
            }
            return;
        }
        case ARPILE_KEY_ESCAPE:
            s->prompt = PMPT_NONE; s->input[0] = 0; s->input_pos = 0;
            s->rename_node = NULL; return;
        case ARPILE_KEY_BACKSPACE:
            if (s->input_pos > 0) {
                s->input_pos--;
                memmove(s->input + s->input_pos, s->input + s->input_pos + 1,
                        strlen(s->input + s->input_pos + 1) + 1);
            }
            return;
        case ARPILE_KEY_LEFT:
            if (s->input_pos > 0) { s->input_pos--; }
            return;
        case ARPILE_KEY_RIGHT:
            if (s->input_pos < (int)strlen(s->input)) { s->input_pos++; }
            return;
        default: break;
    }
    if (ev->key.ascii >= 0x20 && ev->key.ascii != 0x7f) {
        int len = strlen(s->input);
        if (len < 119) {
            memmove(s->input + s->input_pos + 1, s->input + s->input_pos,
                    len - s->input_pos + 1);
            s->input[s->input_pos] = ev->key.ascii;
            s->input_pos++;
        }
    }
}

static void code_mouse(arpile_app_ctx_t *ctx, const arpile_input_event_t *ev)
{
    code_state_t *s = ctx->user;
    if (!s) return;
    ui_rect_t c = win_client_rect(ctx->win);
    int mx = ev->mouse.x, my = ev->mouse.y;
    bool left = (ev->mouse.buttons & ARPILE_MOUSE_BTN_LEFT);
    bool press = (ev->type == ARPILE_IN_EVENT_MOUSE_BTN) && left;

    int y1 = c.y + CODE_TAB_H;
    int out_h = s->out_open ? CODE_OUT_H : 0;
    int edit_h = c.h - CODE_TAB_H - out_h;
    if (edit_h < CODE_LH) edit_h = CODE_LH;

    /* tabs band */
    if (my >= c.y && my < y1) {
        if (press) {
            int tx[CODE_MAX_TABS], tw[CODE_MAX_TABS], tcx[CODE_MAX_TABS];
            code_tab_layout(s, c.x, tx, tw, tcx);
            for (int i = 0; i < s->tab_n; i++) {
                if (mx >= tx[i] && mx <= tx[i] + tw[i]) {
                    if (mx >= tcx[i] - 8) close_tab(s, i);
                    else s->tab_active = i;
                    break;
                }
            }
        }
        return;
    }
    /* output area */
    if (out_h && my >= y1 + edit_h) {
        s->pane = PANE_OUTPUT;
        return;
    }
    /* explorer */
    if (mx < c.x + CODE_EXPL_W) {
        s->pane = PANE_EXPLORER;
        if (press) {
            int row = (my - (y1 + CODE_LH)) / CODE_LH + s->expl_scroll;
            if (row >= 0 && row < s->vis_n && my >= y1 + CODE_LH) {
                code_node_t *n = s->vis[row];
                bool was_sel = (row == s->expl_sel);
                if (n->is_dir && was_sel) n->expanded = !n->expanded;
                else s->expl_sel = row;
                explorer_build(s);
                if (n && !n->is_dir) { open_file_in_tab(s, n->path); s->pane = PANE_EDITOR; }
            }
        }
        return;
    }
    /* editor */
    s->pane = PANE_EDITOR;
    if (press) {
        code_tab_t *t = act_tab(s);
        if (!t) { new_untitled_tab(s); t = act_tab(s); }
        int gx = c.x + CODE_EXPL_W + CODE_GUTTER;
        if (mx >= gx && my >= y1 && my <= y1 + edit_h) {
            int col = (mx - gx) / CHAR_W + t->scroll_x;
            int row = (my - y1) / CODE_LH + t->scroll_y;
            int tot = total_lines(t);
            if (row < tot) {
                int a = line_offset_of(t, row);
                int b = line_end_of(t, a);
                int off = a + col; if (off > b) off = b;
                t->cursor = off; t->anchor = -1;
            }
        }
    }
}

static void code_event(arpile_app_ctx_t *ctx, const arpile_input_event_t *ev)
{
    code_state_t *s = ctx->user;
    if (!s) return;

    if (ev->type == ARPILE_IN_EVENT_MOUSE_BTN ||
        ev->type == ARPILE_IN_EVENT_MOUSE_MOVE) {
        code_mouse(ctx, ev);
        arpile_ui_win_redraw(ctx->win);
        return;
    }
    if (ev->type == ARPILE_IN_EVENT_MOUSE_WHEEL) {
        if (s->pane == PANE_OUTPUT) {
            s->out.scroll += (ev->wheel > 0) ? 1 : -1;
            if (s->out.scroll < 0) s->out.scroll = 0;
        } else if (s->pane == PANE_EDITOR) {
            code_tab_t *t = act_tab(s);
            if (t) { t->scroll_y += (ev->wheel > 0) ? -2 : 2;
                     if (t->scroll_y < 0) t->scroll_y = 0; }
        }
        arpile_ui_win_redraw(ctx->win);
        return;
    }
    if (ev->type != ARPILE_IN_EVENT_KEY_DOWN) return;

    uint8_t m = ev->key.modifier;
    bool ctrl = (m & (ARPILE_MOD_LCTRL | ARPILE_MOD_RCTRL)) != 0;
    if (s->prompt != PMPT_NONE) {
        code_input_key(s, ev);
        arpile_ui_win_redraw(ctx->win);
        return;
    }
    if (ctrl) {
        if (ev->key.keycode == 0x13) { /* Ctrl+P command */
            s->prompt = PMPT_CMD; s->input_pos = 0; s->input[0] = 0;
            strncpy(s->prompt_label, ">", sizeof(s->prompt_label) - 1);
            arpile_ui_win_redraw(ctx->win); return;
        }
        if (ev->key.keycode == 0x05) { /* Ctrl+B toggle output */
            s->out_open = !s->out_open;
            arpile_ui_win_redraw(ctx->win); return;
        }
    }
    if (s->pane == PANE_EDITOR) {
        code_tab_t *t = act_tab(s);
        if (!t) { new_untitled_tab(s); t = act_tab(s); }
        code_key_editor(s, t, ev);
    } else if (s->pane == PANE_EXPLORER) {
        code_key_explorer(s, ev);
    } else {
        code_key_output(s, ev);
    }
    arpile_ui_win_redraw(ctx->win);
}

/* ------------------------------------------------------------------ */
/* Lifecycle + descriptor                                              */
/* ------------------------------------------------------------------ */
static void code_init(arpile_app_ctx_t *ctx)
{
    code_state_t *s = calloc(1, sizeof(code_state_t));
    if (!s) return;
    s->vis = NULL; s->vis_cap = 0; s->vis_n = 0;
    s->expl_scroll = 0; s->expl_rows = 0;
    s->root = calloc(1, sizeof(code_node_t));
    if (!s->root) { free(s); return; }
    strncpy(s->root->name, base_name(CODE_ROOT), 63); s->root->name[63] = 0;
    strncpy(s->root->path, CODE_ROOT, 255); s->root->path[255] = 0;
    s->root->is_dir = true; s->root->expanded = true; s->root->loaded = false;
    s->root->parent = NULL;
    strncpy(s->exec_cwd, CODE_ROOT, 255); s->exec_cwd[255] = 0;
    s->out_open = true;
    s->pane = PANE_EDITOR;
    s->prompt = PMPT_NONE;
    s->expl_sel = 0;
    s->out.len = 0; s->out.scroll = 0;
    explorer_build(s);
    new_untitled_tab(s);
    strncpy(s->msg, "Ctrl+O explorer  Ctrl+P run  Ctrl+S save  ` toggle pane",
            sizeof(s->msg));
    ctx->user = s;
    if (ctx->win) ctx->win->state.capture_nav = true;
}

static void code_destroy(arpile_app_ctx_t *ctx)
{
    code_state_t *s = ctx->user;
    if (!s) return;
    for (int i = 0; i < s->tab_n; i++) tab_free(&s->tabs[i]);
    if (s->root) { node_free(s->root); free(s->root); }
    free(s->vis);
    free(s);
    ctx->user = NULL;
}

static const arpile_app_ops_t code_ops = {
    .init = code_init,
    .update = NULL,
    .event = code_event,
    .render = code_render,
    .destroy = code_destroy,
};

const arpile_app_t arpile_app_editor = {
    .id = "editor",
    .name = "Code",
    .icon = "vscode",
    .ops = &code_ops,
};
