#include <string.h>
#include <stdio.h>
#include "arpile_ui_widgets.h"
#include "usb_host_input.h"

void ui_paint_button(ili9488_t *lcd, const ui_button_t *b)
{
    uint16_t bg = b->down ? UI_C_BUTTON_DOWN : UI_C_BUTTON;
    ui_draw_fill_rect(lcd, &b->rect, bg);
    ui_draw_outline(lcd, &b->rect, b->focused ? UI_C_ACCENT : UI_C_BORDER);

    /* Safety: never draw a label wider than the button frame. */
    char label[32];
    snprintf(label, sizeof(label), "%s", b->label);
    const int max_w = (int)b->rect.w - 8;
    while (label[0] && (int)ui_text_width(label) > max_w) {
        label[strlen(label) - 1] = 0;
    }

    uint16_t tw = ui_text_width(label);
    uint16_t tx = (uint16_t)(b->rect.x + ((int)b->rect.w - tw) / 2);
    uint16_t ty = (uint16_t)(b->rect.y + ((int)b->rect.h - CHAR_H) / 2);
    ui_draw_text(lcd, tx, ty, label, UI_C_TEXT, bg);
}

void ui_paint_panel(ili9488_t *lcd, const ui_panel_t *p)
{
    ui_draw_fill_rect(lcd, &p->rect, UI_C_WIN_BG);
    ui_draw_outline(lcd, &p->rect, UI_C_BORDER);
    ui_panel_t t = *p;
    t.rect.h = 16;
    ui_draw_fill_rect(lcd, &t.rect, UI_C_WIN_TITLE_I);
    ui_draw_text(lcd, (uint16_t)(p->rect.x + 4), (uint16_t)(p->rect.y + 3),
                 p->title, UI_C_TEXT_LIGHT, UI_C_WIN_TITLE_I);
}

void ui_paint_group(ili9488_t *lcd, const ui_rect_t *r)
{
    ui_draw_fill_rect(lcd, r, UI_C_WIN_BG);
    ui_draw_outline(lcd, r, UI_C_BORDER);
}

void ui_paint_scroll_edges(ili9488_t *lcd, const ui_scroll_t *s)
{
    /* Simple visibility indicator: one box if content exceeds viewport. */
    bool overflow = s->view_h > (s->bottom - s->top - s->scroll_y);
    if (!overflow) {
        return;
    }
    uint16_t bar_h = 10;
    uint16_t sx = (uint16_t)(s->right - 4);
    uint16_t sy = (uint16_t)(s->top + 2);
    const ui_rect_t r = { sx, (uint16_t)(s->bottom - bar_h - 2),
                          (uint16_t)(s->right - s->left + 4), bar_h };
    ui_draw_fill_rect(lcd, &r, UI_C_PANEL_HI);
    (void)sy;
}

void ui_paint_list(ili9488_t *lcd, const ui_rect_t *r,
                   const char *const *rows, int count, int sel)
{
    int row_h = CHAR_H + 6;
    int y = r->y + 2;
    for (int i = 0; i < count; i++) {
        if (y + row_h > (int)(r->y + r->h)) {
            break;
        }
        if (i == sel) {
            uint16_t h = (uint16_t)row_h;
            if (y + h > (int)(r->y + r->h)) h = (uint16_t)((int)(r->y + r->h) - y);
            const ui_rect_t selr = { (uint16_t)(r->x + 1), (uint16_t)y,
                                     (uint16_t)(r->w - 2), h };
            ui_draw_fill_rect(lcd, &selr, UI_C_ACCENT);
            ui_draw_text(lcd, (uint16_t)(r->x + 4), (uint16_t)(y + 3),
                         rows[i], UI_C_TEXT_LIGHT, UI_C_ACCENT);
        } else {
            ui_draw_text(lcd, (uint16_t)(r->x + 4), (uint16_t)(y + 3),
                         rows[i], UI_C_TEXT, UI_C_WIN_BG);
        }
        y += row_h;
    }
}

void ui_paint_dialog(ili9488_t *lcd, const ui_dialog_t *d,
                     const char *message, const char *btn_ok, const char *btn_cancel)
{    ui_draw_fill_rect(lcd, &(ui_rect_t){ 0, 0, UI_W, UI_H },
                      UI_RGB(0, 0, 0));
    ui_draw_fill_rect(lcd, &d->rect, UI_C_WIN_BG);
    ui_draw_outline(lcd, &d->rect, UI_C_ACCENT);
    /* dialog title bar */
    ui_rect_t title = { d->rect.x, d->rect.y, d->rect.w, 16 };
    ui_draw_fill_rect(lcd, &title, UI_C_WIN_TITLE);
    ui_draw_text(lcd, (uint16_t)(d->rect.x + 4), (uint16_t)(d->rect.y + 3),
                 d->title, UI_C_TEXT_LIGHT, UI_C_WIN_TITLE);

    /* message, word-wrapped simply */
    int x = d->rect.x + 6, y = d->rect.y + 22;
    const char *msg = message;
    int box_w = (int)(d->rect.w) - 12;
    while (*msg) {
        /* cut at nearest boundary <= box_w */
        const char *sp = msg + (box_w / CHAR_W);
        if (*sp) {
            while (sp > msg && *sp != ' ') sp--;
            if (sp == msg) sp = msg + (box_w / CHAR_W);
        }
        char tmp[80];
        int len = (int)(sp - msg);
        if (len > (int)sizeof(tmp) - 1) len = (int)sizeof(tmp) - 1;
        memcpy(tmp, msg, (size_t)len);
        tmp[len] = 0;
        ui_draw_text(lcd, (uint16_t)x, (uint16_t)y, tmp, UI_C_TEXT, UI_C_WIN_BG);
        y += CHAR_H + 2;
        while (*sp == ' ') sp++;
        if (*sp) msg = sp; else break;
        if (y > (int)(d->rect.y + d->rect.h - 24)) break;
    }

    /* buttons */
    int bw = 44, bh = 14;
    int by = (int)(d->rect.y + d->rect.h) - bh - 6;
    if (btn_cancel) {
        ui_button_t bc = { ui_rect((uint16_t)((int)d->rect.x + d->rect.w - bw - 6),
                                   (uint16_t)by, (uint16_t)bw, (uint16_t)bh),
                           btn_cancel, false, false };
        ui_paint_button(lcd, &bc);
    }
    if (btn_ok) {
        ui_button_t bo = { ui_rect((uint16_t)((int)d->rect.x + d->rect.w - bw * 2 - 14),
                                   (uint16_t)by, (uint16_t)bw, (uint16_t)bh),
                           btn_ok, false, false };
        ui_paint_button(lcd, &bo);
    }
}
/* ---------------- app kit: shared navigation + chrome ---------------- */

bool ui_kit_list_nav(int *sel, int *top, int view_rows, int count,
                     uint16_t keycode)
{
    if (count <= 0) {
        return false;
    }
    switch (keycode) {
    case ARPILE_KEY_UP:    (*sel)--; break;
    case ARPILE_KEY_DOWN:  (*sel)++; break;
    case ARPILE_KEY_PGUP:  (*sel) -= view_rows; break;
    case ARPILE_KEY_PGDN:  (*sel) += view_rows; break;
    case ARPILE_KEY_HOME:  *sel = 0; break;
    case ARPILE_KEY_END:   *sel = count - 1; break;
    default: return false;
    }
    if (*sel < 0) *sel = 0;
    if (*sel > count - 1) *sel = count - 1;

    /* keep selection inside the visible window */
    if (*sel < *top) {
        *top = *sel;
    } else if (*sel > *top + view_rows - 1) {
        *top = *sel - view_rows + 1;
    }
    if (*top > count - view_rows) {
        *top = count - view_rows;
    }
    if (*top < 0) {
        *top = 0;
    }
    return true;   /* nav keys are always consumed */
}

void ui_kit_list_paint(ili9488_t *lcd, const ui_rect_t *r, int row_h,
                       int top, int sel, int count,
                       ui_kit_row_fn get_label, void *user)
{
    int rows = (r->h - 2) / row_h;
    for (int i = 0; i < rows; i++) {
        int idx = top + i;
        if (idx >= count) {
            break;
        }
        const char *label = get_label(idx, user);
        if (!label) {
            continue;
        }
        int y = r->y + 2 + i * row_h;
        bool is_sel = (idx == sel);
        char line[64];
        snprintf(line, sizeof(line), "%.*s", (int)(sizeof(line) - 1), label);
        uint16_t tw = ui_text_width(line);
        if ((uint16_t)tw > r->w - 10) {
            /* clip long labels to fit */
            line[0] = 0;
            for (int ch = 0; ch < (int)(sizeof(line) - 1) && label[ch]; ch++) {
                line[ch] = label[ch];
                line[ch + 1] = 0;
                if (ui_text_width(line) > r->w - 16) {
                    line[ch] = 0;
                    break;
                }
            }
        }
        if (is_sel) {
            const ui_rect_t selr = { (uint16_t)(r->x + 1), (uint16_t)y,
                                     (uint16_t)(r->w - 2), (uint16_t)row_h };
            ui_draw_fill_rect(lcd, &selr, UI_C_ACCENT);
            ui_draw_text(lcd, (uint16_t)(r->x + 4), (uint16_t)(y + 3),
                         line, UI_C_TEXT_LIGHT, UI_C_ACCENT);
        } else {
            ui_draw_text(lcd, (uint16_t)(r->x + 4), (uint16_t)(y + 3),
                         line, UI_C_TEXT, UI_C_WIN_BG);
        }
    }
}

void ui_kit_tabbar(ili9488_t *lcd, const ui_rect_t *r,
                   const char *const *tabs, int count, int active)
{
    uint16_t x = r->x;
    for (int i = 0; i < count; i++) {
        uint16_t tw = ui_text_width(tabs[i]);
        ui_rect_t tr = { x, r->y, (uint16_t)(tw + 12), r->h };
        uint16_t bg = (i == active) ? UI_C_ACCENT : UI_C_BUTTON;
        ui_draw_fill_rect(lcd, &tr, bg);
        ui_draw_outline(lcd, &tr, UI_C_BORDER);
        ui_draw_text(lcd, (uint16_t)(tr.x + 6),
                     (uint16_t)(tr.y + (tr.h - CHAR_H) / 2),
                     tabs[i], UI_C_TEXT, bg);
        x = (uint16_t)(x + tr.w + 2);
    }
}

bool ui_kit_tabbar_hit(const ui_rect_t *r, const char *const *tabs, int count,
                       int mx, int my, int *out_tab_i)
{
    if (my < r->y || my >= (int)(r->y + r->h)) {
        return false;
    }
    uint16_t x = r->x;
    for (int i = 0; i < count; i++) {
        uint16_t w = (uint16_t)(ui_text_width(tabs[i]) + 14);
        if (mx >= x && mx < (int)(x + w)) {
            if (out_tab_i) {
                *out_tab_i = i;
            }
            return true;
        }
        x = (uint16_t)(x + w + 2);
    }
    return false;
}

void ui_kit_statusbar(ili9488_t *lcd, const ui_rect_t *r,
                      const char *left, const char *right)
{
    ui_rect_t bar = { r->x, (uint16_t)(r->y + r->h - CHAR_H - 6),
                      r->w, (uint16_t)(CHAR_H + 6) };
    ui_draw_fill_rect(lcd, &bar, UI_C_WIN_TITLE_I);
    if (left) {
        ui_draw_text(lcd, (uint16_t)(bar.x + 4),
                     (uint16_t)(bar.y + (bar.h - CHAR_H) / 2),
                     left, UI_C_TEXT_LIGHT, UI_C_WIN_TITLE_I);
    }
    if (right) {
        uint16_t tw = ui_text_width(right);
        ui_draw_text(lcd, (uint16_t)(bar.x + bar.w - tw - 4),
                     (uint16_t)(bar.y + (bar.h - CHAR_H) / 2),
                     right, UI_C_TEXT_LIGHT, UI_C_WIN_TITLE_I);
    }
}

/* Value column for kv rows: just past the key plus a gap, but never left
 * of a shared indent so short keys still align. */
static int kv_value_x(int x, const char *key)
{
    int vx = x + (int)ui_text_width(key) + 14;
    int min_vx = x + 104;
    return vx > min_vx ? vx : min_vx;
}

int ui_kit_kv(ili9488_t *lcd, int x, int y, const char *key, const char *value)
{
    char line[80];
    snprintf(line, sizeof(line), "%.24s", key);
    ui_draw_text(lcd, (uint16_t)x, (uint16_t)y, line, UI_C_TEXT_DIM, UI_C_WIN_BG);
    snprintf(line, sizeof(line), "%.40s", value ? value : "");
    ui_draw_text(lcd, (uint16_t)kv_value_x(x, key), (uint16_t)y, line,
                 UI_C_TEXT, UI_C_WIN_BG);
    return y + CHAR_H + 6;
}

int ui_kit_kv_wrap(ili9488_t *lcd, int x, int y, const char *key,
                   const char *value, int max_w)
{
    char kbuf[26];
    snprintf(kbuf, sizeof(kbuf), "%.24s", key);
    ui_draw_text(lcd, (uint16_t)x, (uint16_t)y, kbuf, UI_C_TEXT_DIM,
                 UI_C_WIN_BG);

    if (!value || !*value) {
        return y + CHAR_H + 6;
    }
    int vx = kv_value_x(x, key);
    int avail = x + max_w - vx;
    if (avail < 10 * CHAR_W) {
        avail = 10 * CHAR_W;
    }

    /* Greedy word wrap: fill one line at a time within `avail`. */
    const char *p = value;
    while (*p) {
        char line[64];
        size_t cur = 0;
        line[0] = 0;
        for (;;) {
            while (*p == ' ') {
                p++;
            }
            if (!*p) {
                break;
            }
            const char *w = p;
            while (*p && *p != ' ') {
                p++;
            }
            size_t wl = (size_t)(p - w);
            size_t add = cur ? wl + 1 : wl;
            bool fits = ((int)(cur + add) * CHAR_W <= avail);
            if (cur > 0 && !fits) {
                p = w;   /* word goes to the next line */
                break;
            }
            if (cur + add >= sizeof(line)) {
                break;   /* line buffer full */
            }
            if (cur) {
                line[cur++] = ' ';
            }
            memcpy(line + cur, w, wl);
            cur += wl;
            line[cur] = 0;
        }
        if (!line[0]) {
            break;
        }
        ui_draw_text(lcd, (uint16_t)vx, (uint16_t)y, line, UI_C_TEXT,
                     UI_C_WIN_BG);
        y += CHAR_H + 2;
    }
    return y + CHAR_H + 4;
}

int ui_kit_header(ili9488_t *lcd, int x, int y, const char *title)
{
    ui_draw_text(lcd, (uint16_t)x, (uint16_t)y, title, UI_C_TEXT_LIGHT, UI_C_WIN_BG);
    uint16_t tw = ui_text_width(title);
    const ui_rect_t ul = { (uint16_t)x, (uint16_t)(y + CHAR_H + 1),
                           (uint16_t)(tw + 4), 2 };
    ui_draw_fill_rect(lcd, &ul, UI_C_ACCENT);
    return y + CHAR_H + 8;
}
