#include "arpile_ui.h"
#include "arpile_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#define MATH_HIST_MAX 50
#define MATH_INPUT_MAX 256
#define MATH_GRAPH_W 240
#define MATH_GRAPH_H 240

typedef struct {
    char text[MATH_INPUT_MAX];
    double result;
    bool valid;
} math_hist_entry_t;

typedef struct {
    math_hist_entry_t hist[MATH_HIST_MAX];
    int hist_len;
    int hist_scroll;
    char input[MATH_INPUT_MAX];
    int input_len;
    int cursor;
    int hist_cursor; // for history navigation (-1 none)
    bool show_help;
} math_state_t;

/* Simple expression evaluator supporting + - * / ^ parentheses and functions sin, cos, tan, sqrt, log, exp */
typedef struct { const char *s; } parser_t;
static double parse_expr(parser_t *p);
static double parse_term(parser_t *p);
static double parse_factor(parser_t *p);
static double parse_power(parser_t *p);
static double parse_primary(parser_t *p);
static void skip_spaces(parser_t *p) { while (*p->s && isspace((unsigned char)*p->s)) p->s++; }
static double parse_number(parser_t *p) { double v = strtod(p->s, (char **)&p->s); return v; }
static double parse_primary(parser_t *p) {
    skip_spaces(p);
    if (*p->s == '(') { p->s++; double v = parse_expr(p); skip_spaces(p); if (*p->s == ')') p->s++; return v; }
    if (isalpha((unsigned char)*p->s)) {
        char name[32]; int i=0;
        while (*p->s && (isalnum((unsigned char)*p->s) || *p->s=='_')) { if (i<31) name[i++]=*p->s; p->s++; }
        name[i]=0; skip_spaces(p);
        if (*p->s=='(') { p->s++; double arg=parse_expr(p); skip_spaces(p); if (*p->s==')') p->s++;
            if (!strcmp(name,"sin")) return sin(arg);
            if (!strcmp(name,"cos")) return cos(arg);
            if (!strcmp(name,"tan")) return tan(arg);
            if (!strcmp(name,"sqrt")) return sqrt(arg);
            if (!strcmp(name,"log")) return log(arg);
            if (!strcmp(name,"exp")) return exp(arg);
            if (!strcmp(name,"abs")) return fabs(arg);
            return 0;
        }
        if (!strcmp(name,"pi")) return M_PI;
        if (!strcmp(name,"e")) return M_E;
        return 0;
    }
    return parse_number(p);
}
static double parse_power(parser_t *p) {
    double left = parse_primary(p);
    skip_spaces(p);
    if (*p->s=='^') { p->s++; double right = parse_power(p); return pow(left,right); }
    return left;
}
static double parse_factor(parser_t *p) {
    double left = parse_power(p);
    skip_spaces(p);
    while (*p->s=='*' || *p->s=='/') {
        char op=*p->s++; double right=parse_power(p);
        if (op=='*') left*=right; else left/=right;
        skip_spaces(p);
    }
    return left;
}
static double parse_term(parser_t *p) {
    double left = parse_factor(p);
    skip_spaces(p);
    while (*p->s=='+' || *p->s=='-') {
        char op=*p->s++; double right=parse_factor(p);
        if (op=='+') left+=right; else left-=right;
        skip_spaces(p);
    }
    return left;
}
static double parse_expr(parser_t *p) { return parse_term(p); }

static double eval_expr(const char *s) {
    parser_t p={s}; double v=parse_expr(&p); return v;
}

/* UI drawing helpers */
static void draw_graph(ili9488_t *lcd, const ui_rect_t *r) {
    ui_draw_fill_rect(lcd, r, UI_C_WIN_BG);
    ui_draw_outline(lcd, r, UI_C_BORDER);
    int cx = r->x + r->w/2;
    int cy = r->y + r->h/2;
    ui_draw_hline(lcd, r->x, cy, r->w, UI_C_BORDER);
    ui_draw_vline(lcd, cx, r->y, r->h, UI_C_BORDER);
    /* simple grid */
    for (int x=r->x;x<r->x+r->w;x+=20) ui_draw_vline(lcd,x,r->y,r->h,UI_C_TEXT_DIM);
    for (int y=r->y;y<r->y+r->h;y+=20) ui_draw_hline(lcd,r->x,y,r->w,UI_C_TEXT_DIM);
}

static bool is_simple_fraction(const char *txt, char **num, char **den) {
    const char *slash = strchr(txt, '/');
    if (!slash) return false;
    if (strchr(slash+1, '/')) return false; /* more than one slash */
    for (const char *p=txt; p<slash; ++p) if (!isdigit((unsigned char)*p) && *p!='.' && *p!='-' && *p!='+') return false;
    for (const char *p=slash+1; *p; ++p) if (!isdigit((unsigned char)*p) && *p!='.' && *p!='-' && *p!='+') return false;
    *num = strndup(txt, slash - txt);
    *den = strdup(slash+1);
    return true;
}

static void draw_history(ili9488_t *lcd, const ui_rect_t *r, math_state_t *s) {
    ui_draw_fill_rect(lcd, r, UI_C_WIN_BG);
    ui_draw_outline(lcd, r, UI_C_BORDER);
    int line_h = CHAR_H + 2;
    int max_lines = r->h / line_h;
    int start = s->hist_len - max_lines - s->hist_scroll;
    if (start < 0) start = 0;
    int y = r->y + 2;
    for (int i=start; i<s->hist_len && y+line_h<=r->y+r->h; ++i) {
        const math_hist_entry_t *e = &s->hist[i];
        char *num=NULL, *den=NULL;
        if (e->valid && is_simple_fraction(e->text, &num, &den)) {
            /* draw numerator */
            ui_draw_text(lcd, r->x+4, y, num, UI_C_TEXT, UI_C_WIN_BG);
            y += line_h;
            /* draw fraction bar */
            ui_draw_hline(lcd, r->x+4, y, (uint16_t)(r->w-8), UI_C_BORDER);
            y += line_h;
            /* draw denominator */
            ui_draw_text(lcd, r->x+4, y, den, UI_C_TEXT, UI_C_WIN_BG);
            y += line_h;
            free(num); free(den);
        } else {
            char buf[256];
            if (e->valid) {
                int n = snprintf(buf, sizeof(buf), "> %s = %.10g", e->text, e->result);
                if (n >= (int)sizeof(buf)) buf[sizeof(buf)-1]=0;
            } else {
                int n = snprintf(buf, sizeof(buf), "> %s = Error", e->text);
                if (n >= (int)sizeof(buf)) buf[sizeof(buf)-1]=0;
            }
            ui_draw_text(lcd, r->x+4, y, buf, UI_C_TEXT, UI_C_WIN_BG);
            y += line_h * 2;
        }
    }
}

static void draw_input(ili9488_t *lcd, const ui_rect_t *r, math_state_t *s) {
    ui_draw_fill_rect(lcd, r, UI_C_WIN_BG);
    ui_draw_outline(lcd, r, UI_C_BORDER);
    char prompt[512];
    int n = snprintf(prompt, sizeof(prompt), "> %s", s->input);
    if (n >= (int)sizeof(prompt)) prompt[sizeof(prompt)-1]=0;
    ui_draw_text(lcd, r->x+4, r->y+2, prompt, UI_C_TEXT, UI_C_WIN_BG);
    /* cursor */
    int cx = r->x + 4 + (int)ui_text_width(prompt) - (int)ui_text_width(s->input + s->cursor);
    ui_draw_vline(lcd, cx, r->y+2, CHAR_H, UI_C_ACCENT);
}

/* App callbacks */
static void math_init(arpile_app_ctx_t *ctx) {
    math_state_t *st = malloc(sizeof(math_state_t));
    if (!st) {
        /* fallback: try smaller allocation */
        st = malloc(sizeof(math_state_t));
    }
    if (!st) return;
    memset(st, 0, sizeof(math_state_t));
    ctx->user = st;
    if (ctx->win) ctx->win->state.capture_nav = true;
}

static void math_destroy(arpile_app_ctx_t *ctx) {
    math_state_t *st = ctx->user;
    free(st);
    ctx->user = NULL;
}

static void math_render(arpile_app_ctx_t *ctx, ui_win_t *win) {
    math_state_t *st = ctx->user;
    ili9488_t *lcd = arpile_ui_get_lcd();
    ui_rect_t c = win_client_rect(win);
    if (!st) {
        /* Lazy init if the normal init path missed */
        math_init(ctx);
        st = ctx->user;
        if (!st) {
            ui_draw_fill_rect(lcd, &c, 0xF800);
            ui_draw_text(lcd, c.x + 4, c.y + 4, "MATH: init failed",
                         UI_C_TEXT_LIGHT, 0xF800);
            return;
        }
    }
    ui_draw_fill_rect(lcd, &c, UI_C_WIN_BG);

    ui_rect_t graph = { c.x, c.y, MATH_GRAPH_W, c.h - 60 };
    ui_rect_t hist  = { c.x + MATH_GRAPH_W, c.y, c.w - MATH_GRAPH_W, c.h - 60 };
    ui_rect_t inp   = { c.x, c.y + c.h - 50, c.w, 40 };

    draw_graph(lcd, &graph);
    draw_history(lcd, &hist, st);
    draw_input(lcd, &inp, st);
    if (st->show_help) {
        int bw = 300, bh = 200;
        int bx = c.x + (c.w - bw) / 2;
        int by = c.y + (c.h - bh) / 2;
        ui_rect_t box = { (uint16_t)bx, (uint16_t)by, (uint16_t)bw, (uint16_t)bh };
        ui_draw_fill_rect(lcd, &box, UI_C_PANEL);
        ui_draw_outline(lcd, &box, UI_C_ACCENT);
        const char *lines[] = {
            "Shortcuts (Ctrl+Shift):",
            "  P   %   percentile",
            "  O   pi  pi constant",
            "  R   sqrt(  square root",
            "  C   ^   power",
            "  D   /   division",
            "",
            "Other:",
            "  Enter   evaluate",
            "  Up/Down history",
            "  Ctrl+L  clear history",
            "  F7      toggle this help",
            "  F10     fullscreen",
        };
        int y = by + 6;
        for (size_t i=0;i<sizeof(lines)/sizeof(lines[0]);++i) {
            ui_draw_text(lcd, bx + 8, y, lines[i], UI_C_TEXT, UI_C_PANEL);
            y += CHAR_H + 2;
        }
    }
}

/* Evaluate current input, push to history */
static void math_eval(math_state_t *st) {
    if (st->input_len==0) return;
    math_hist_entry_t *e = &st->hist[st->hist_len % MATH_HIST_MAX];
    strncpy(e->text, st->input, MATH_INPUT_MAX-1);
    e->text[MATH_INPUT_MAX-1]=0;
    parser_t p={st->input};
    double val=0; bool ok=true;
    val = parse_expr(&p);
    e->result = val;
    e->valid = ok;
    st->hist_len++;
    /* reset input */
    st->input[0]=0; st->input_len=0; st->cursor=0;
    st->hist_scroll=0;
    st->hist_cursor=-1;
}

static void math_event(arpile_app_ctx_t *ctx, const arpile_input_event_t *ev) {
    math_state_t *st = ctx->user;
    if (!st) return;
    if (ev->type != ARPILE_IN_EVENT_KEY_DOWN) return;
    uint16_t key = ev->key.keycode;
    uint8_t mod = ev->key.modifier;
    bool ctrl = mod & (ARPILE_MOD_LCTRL|ARPILE_MOD_RCTRL);
    if (key == ARPILE_KEY_F7) {
        st->show_help = !st->show_help;
        arpile_ui_win_redraw(ctx->win);
        return;
    }
    if (ctrl && key==0x0C) { /* Ctrl+L clear history */
        st->hist_len=0; st->hist_scroll=0; arpile_ui_win_redraw(ctx->win); return;
    }
    bool shift = mod & (ARPILE_MOD_LSHIFT|ARPILE_MOD_RSHIFT);
    if (ctrl && shift) {
        const char *sym = NULL;
        switch (key) {
            case 0x13: sym = "%"; break;          /* Ctrl+Shift+P -> % */
            case 0x12: sym = "pi"; break;         /* Ctrl+Shift+O -> pi */
            case 0x15: sym = "sqrt("; break;      /* Ctrl+Shift+R -> sqrt( */
            case 0x06: sym = "^"; break;          /* Ctrl+Shift+C -> ^ */
            case 0x07: sym = "/"; break;          /* Ctrl+Shift+D -> / */
            default: break;
        }
        if (sym) {
            int len = strlen(sym);
            if (st->input_len + len < MATH_INPUT_MAX) {
                memmove(st->input+st->cursor+len, st->input+st->cursor, st->input_len-st->cursor+1);
                memcpy(st->input+st->cursor, sym, len);
                st->cursor += len; st->input_len += len;
                arpile_ui_win_redraw(ctx->win);
            }
            return;
        }
    }
    if (key==ARPILE_KEY_ENTER) {
        math_eval(st);
        arpile_ui_win_redraw(ctx->win);
        return;
    }
    if (key==ARPILE_KEY_BACKSPACE) {
        if (st->cursor>0) {
            memmove(st->input+st->cursor-1, st->input+st->cursor, st->input_len-st->cursor+1);
            st->cursor--; st->input_len--;
            arpile_ui_win_redraw(ctx->win);
        }
        return;
    }
    if (key==ARPILE_KEY_LEFT) {
        if (st->cursor>0) { st->cursor--; arpile_ui_win_redraw(ctx->win); }
        return;
    }
    if (key==ARPILE_KEY_RIGHT) {
        if (st->cursor<st->input_len) { st->cursor++; arpile_ui_win_redraw(ctx->win); }
        return;
    }
    if (key==ARPILE_KEY_UP) {
        if (st->hist_len>0) {
            int idx = (st->hist_len-1 - (st->hist_cursor+1));
            if (idx>=0) {
                st->hist_cursor++;
                math_hist_entry_t *e=&st->hist[idx%MATH_HIST_MAX];
                strncpy(st->input, e->text, MATH_INPUT_MAX-1);
                st->input_len=strlen(st->input);
                st->cursor=st->input_len;
                arpile_ui_win_redraw(ctx->win);
            }
        }
        return;
    }
    if (key==ARPILE_KEY_DOWN) {
        if (st->hist_cursor>0) {
            st->hist_cursor--;
            int idx = (st->hist_len-1 - st->hist_cursor);
            math_hist_entry_t *e=&st->hist[idx%MATH_HIST_MAX];
            strncpy(st->input, e->text, MATH_INPUT_MAX-1);
            st->input_len=strlen(st->input);
            st->cursor=st->input_len;
            arpile_ui_win_redraw(ctx->win);
        }
        return;
    }
    if (ev->key.ascii >= 0x20 && ev->key.ascii < 0x7F) {
        if (st->input_len < MATH_INPUT_MAX-1) {
            memmove(st->input+st->cursor+1, st->input+st->cursor, st->input_len-st->cursor+1);
            st->input[st->cursor]=ev->key.ascii;
            st->cursor++; st->input_len++;
            arpile_ui_win_redraw(ctx->win);
        }
    }
}

static const arpile_app_ops_t math_ops = {
    .init = math_init,
    .event = math_event,
    .render = math_render,
    .destroy = math_destroy,
};

const arpile_app_t arpile_app_math = {
    .id = "math",
    .name = "Math",
    .icon = "math",
    .ops = &math_ops,
};