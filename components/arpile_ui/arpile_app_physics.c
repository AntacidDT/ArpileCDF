#include "arpile_ui.h"
#include "arpile_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PHYS_MAX_SECTORS 10
#define PHYS_MAX_PAGES 5

typedef struct {
    const char *title;
    const char *lines[5];
    int page_count;
} phys_page_t;

typedef struct {
    const char *name;
    phys_page_t pages[5];
    int page_cnt;
} phys_sector_t;

static const phys_sector_t g_phys_sectors[] = {
    {
        "Mechanics",
        {
            {"Motion", {"Motion is change in position over time.", "Velocity = displacement / time.", "Acceleration = change in velocity / time."}, 3},
            {"Newton's Laws", {"1st: Inertia - object stays at rest or uniform motion unless acted on.", "2nd: F = m·a.", "3rd: Action-reaction pairs."}, 3},
            {"Energy & Work", {"Work = force × distance (along force).", "Kinetic energy = ½ m v².", "Potential energy (gravitational) = m g h."}, 3},
            {"Momentum", {"Momentum p = m v.", "Conserved in closed systems.", "Impulse = change in momentum."}, 3},
        },
        4
    },
    {
        "Waves",
        {
            {"Wave Basics", {"Wavelength λ, frequency f, speed v = f λ.", "Amplitude = max displacement.", "Period T = 1/f."}, 3},
            {"Wave Behaviour", {"Reflection, refraction, diffraction, interference.", "Superposition principle."}, 2},
            {"Sound", {"Longitudinal pressure waves.", "Speed depends on medium.", "Pitch ~ frequency; loudness ~ amplitude."}, 3},
        },
        3
    },
    {
        "Optics",
        {
            {"Reflection", {"Angle of incidence = angle of reflection.", "Plane mirrors produce virtual images."}, 2},
            {"Refraction", {"Snell's law: n1 sinθ1 = n2 sinθ2.", "Total internal reflection above critical angle."}, 2},
            {"Lenses & Mirrors", {"Converging lens focuses parallel rays to focal point.", "Lens formula 1/f = 1/v + 1/u."}, 2},
        },
        3
    },
    {
        "Electricity",
        {
            {"Ohm's Law", {"V = I·R.", "Voltage (V), Current (A), Resistance (Ω)."}, 2},
            {"Power", {"P = V·I = I²·R = V²/R."}, 1},
            {"Circuits", {"Series: R_total = R1+R2+…", "Parallel: 1/R_total = Σ 1/Ri."}, 2},
        },
        3
    },
    {
        "Magnetism",
        {
            {"Magnetic Fields", {"Field lines from N to S.", "Force on moving charge: F = q v × B."}, 2},
            {"Electromagnetism", {"Current creates circular B-field.", "Force on wire: F = I L × B."}, 2},
            {"Induction", {"Faraday's law: emf = -dΦ/dt.", "Lenz's law opposes change."}, 2},
        },
        3
    },
    {
        "Thermodynamics",
        {
            {"Temperature & Heat", {"Temperature measures average kinetic energy.", "Heat flows from hot to cold."}, 2},
            {"Laws", {"0th: thermal equilibrium.", "1st: ΔU = Q - W.", "2nd: entropy of isolated system ↑.", "3rd: S→0 as T→0 K."}, 4},
            {"Gas Laws", {"Ideal gas: PV = nRT.", "Boyle, Charles, Gay‑Lussac."}, 3},
        },
        3
    },
    {
        "Nuclear Physics",
        {
            {"Radioactivity", {"α, β, γ decay.", "Half‑life T½ = ln2 / λ."}, 2},
            {"Fission & Fusion", {"Fission splits heavy nuclei.", "Fusion combines light nuclei, powers stars."}, 2},
        },
        2
    },
    {
        "Relativity",
        {
            {"Special Relativity", {"c is constant in all inertial frames.", "Time dilation: t = γ t₀.", "Length contraction: L = L₀/γ.", "E = γ m c²."}, 4},
        },
        1
    },
    {
        "Modern Physics",
        {
            {"Quantum Basics", {"Energy quantized: E = h f.", "Wave‑particle duality.", "Uncertainty principle Δx Δp ≥ ħ/2."}, 3},
        },
        1
    },
{
            "Constants",
            {
                {"Useful Constants", {"c = 299792458 m/s", "G = 6.67430e-11 N m^2/kg^2", "h = 6.62607015e-34 J s", "hbar = 1.054571817e-34 J s", "e = 1.602176634e-19 C"}, 1},
            },
            1
        },
};
#define PHYS_SECTOR_COUNT (sizeof(g_phys_sectors)/sizeof(g_phys_sectors[0]))

typedef struct {
    int sector;
    int page;
    int scroll;
    bool show_help;
} phys_state_t;

static void draw_wrapped_line(ili9488_t *lcd, int x, int *y, const char *text,
                               uint16_t color, int max_chars, int bottom_limit)
{
    const char *p = text;
    char line[128];
    int line_len = 0;
    while (*p) {
        while (*p == ' ') p++;
        const char *ws = p;
        int wl = 0;
        while (*p && *p != ' ') { p++; wl++; }
        if (!wl) break;
        if (line_len + wl + (line_len?1:0) > max_chars) {
            if (line_len) {
                line[line_len] = 0;
                if (*y + CHAR_H > bottom_limit) return;
                ui_draw_text(lcd, x, *y, line, color, UI_C_WIN_BG);
                *y += CHAR_H + 2;
                line_len = 0;
            }
        }
        if (line_len) line[line_len++] = ' ';
        if (wl > max_chars) {
            for (int i=0;i<wl;i+=max_chars) {
                int chunk = wl - i; if (chunk > max_chars) chunk = max_chars;
                memcpy(line, ws+i, chunk); line[chunk]=0;
                if (*y + CHAR_H > bottom_limit) return;
                ui_draw_text(lcd, x, *y, line, color, UI_C_WIN_BG);
                *y += CHAR_H + 2;
            }
        } else {
            memcpy(line+line_len, ws, wl);
            line_len += wl;
        }
    }
    if (line_len) {
        line[line_len]=0;
        if (*y + CHAR_H <= bottom_limit) {
            ui_draw_text(lcd, x, *y, line, color, UI_C_WIN_BG);
            *y += CHAR_H + 2;
        }
    }
}

static void phys_draw_sectors(ili9488_t *lcd, const ui_rect_t *r, int cur) {
    ui_draw_fill_rect(lcd, r, UI_C_WIN_BG);
    ui_draw_outline(lcd, r, UI_C_BORDER);
    ui_draw_text(lcd, r->x + 4, r->y + 2, "Sectors", UI_C_TEXT_DIM, UI_C_WIN_BG);
    int lh = CHAR_H + 2;
    int max = (r->h - 20) / lh;
    int y = r->y + 18;
    for (int i=0;i<PHYS_SECTOR_COUNT && i<max;i++) {
        if (y + lh > r->y + r->h) break;
        uint16_t col = (i==cur ? UI_C_ACCENT : UI_C_TEXT);
        ui_draw_text(lcd, r->x+4, y, g_phys_sectors[i].name, col, UI_C_WIN_BG);
        y += lh;
    }
}

static void phys_draw_content(ili9488_t *lcd, const ui_rect_t *r, const phys_sector_t *sec,
                               int page, int scroll) {
    ui_draw_fill_rect(lcd, r, UI_C_WIN_BG);
    ui_draw_outline(lcd, r, UI_C_BORDER);
    if (page >= sec->page_cnt) page = sec->page_cnt - 1;
    const phys_page_t *pg = &sec->pages[page];
    int y = r->y + 4;
    int max_chars = (r->w - 12) / CHAR_W;
    if (max_chars < 10) max_chars = 10;
    int bottom_limit = r->y + r->h - 4 - (CHAR_H + 2);

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "%s   %d/%d", pg->title, page+1, pg->page_count);
    ui_draw_text(lcd, r->x + 4, y, hdr, UI_C_TEXT, UI_C_WIN_BG);
    y += CHAR_H + 4;

    for (int i = 0; i < pg->page_count; i++) {
        if (y + CHAR_H > bottom_limit) break;
        draw_wrapped_line(lcd, r->x + 4, &y, pg->lines[i], UI_C_TEXT, max_chars, bottom_limit);
    }

    char pinfo[32];
    snprintf(pinfo, sizeof(pinfo), "PgUp/PgDn  %d/%d", page+1, sec->page_cnt);
    ui_draw_text(lcd, r->x + 4, r->y + r->h - (CHAR_H + 2) - 2, pinfo, UI_C_TEXT_DIM, UI_C_WIN_BG);
}

static void phys_draw_help(ili9488_t *lcd, const ui_rect_t *r) {
    ui_draw_fill_rect(lcd, r, UI_C_PANEL);
    ui_draw_outline(lcd, r, UI_C_ACCENT);
    const char *lines[] = {
        "Physics Shortcuts:",
        "  Tab / Shift+Tab   next / prev sector",
        "  PgDn / PgUp       next / prev page",
        "  Enter             select",
        "  Esc               back / close",
        "  F7                toggle help",
        "  F10               fullscreen",
        "  Mouse wheel       scroll",
        "  Click             select",
    };
    int y = (int)r->y + 6;
    for (size_t i=0;i<sizeof(lines)/sizeof(lines[0]);i++) {
        int ly = y + (int)i * (CHAR_H+2);
        ui_draw_text(lcd, r->x+8, (uint16_t)ly, lines[i], UI_C_TEXT, UI_C_PANEL);
    }
}

static void phys_init(arpile_app_ctx_t *ctx) {
    phys_state_t *st = malloc(sizeof(phys_state_t));
    if (!st) return;
    memset(st,0,sizeof(phys_state_t));
    ctx->user = st;
    if (ctx->win) ctx->win->state.capture_nav = false;
}
static void phys_destroy(arpile_app_ctx_t *ctx) {
    free(ctx->user);
    ctx->user = NULL;
}

static void phys_render(arpile_app_ctx_t *ctx, ui_win_t *win) {
    phys_state_t *st = ctx->user;
    if (!st) { phys_init(ctx); st = ctx->user; if (!st) return; }

    ili9488_t *lcd = arpile_ui_get_lcd();
    ui_rect_t c = win_client_rect(win);
    ui_draw_fill_rect(lcd, &c, UI_C_WIN_BG);

    ui_rect_t left = { c.x, c.y, 180, c.h };
    ui_rect_t right = { (uint16_t)(c.x+180), c.y, (uint16_t)(c.w-180), c.h };

    phys_draw_sectors(lcd, &left, st->sector);
    phys_draw_content(lcd, &right, &g_phys_sectors[st->sector], st->page, st->scroll);

    if (st->show_help) {
        ui_rect_t hp = { c.x+40, c.y+30, 240, 200 };
        phys_draw_help(lcd, &hp);
    }
}

static void phys_event(arpile_app_ctx_t *ctx, const arpile_input_event_t *ev) {
    phys_state_t *st = ctx->user;
    if (!st) return;
    if (ev->type != ARPILE_IN_EVENT_KEY_DOWN) return;
    uint16_t key = ev->key.keycode;
    uint8_t mod = ev->key.modifier;
    bool shift = mod & (ARPILE_MOD_LSHIFT|ARPILE_MOD_RSHIFT);

    if (key == ARPILE_KEY_F7) { st->show_help = !st->show_help; arpile_ui_win_redraw(ctx->win); return; }
    if (key == ARPILE_KEY_F10) return;
    if (key == ARPILE_KEY_ESCAPE) {
        if (st->show_help) { st->show_help=false; arpile_ui_win_redraw(ctx->win); return; }
    }

    if (key == ARPILE_KEY_TAB) {
        st->sector = shift ? (st->sector-1+PHYS_SECTOR_COUNT)%PHYS_SECTOR_COUNT
                           : (st->sector+1)%PHYS_SECTOR_COUNT;
        st->page=0; st->scroll=0;
        arpile_ui_win_redraw(ctx->win); return;
    }
    if (key == ARPILE_KEY_PGDN) {
        if (st->page+1 < g_phys_sectors[st->sector].page_cnt) {
            st->page++; st->scroll=0; arpile_ui_win_redraw(ctx->win);
        }
        return;
    }
    if (key == ARPILE_KEY_PGUP) {
        if (st->page > 0) { st->page--; st->scroll=0; arpile_ui_win_redraw(ctx->win); }
        return;
    }
    if (key == ARPILE_KEY_UP) { if (st->scroll>0) { st->scroll--; arpile_ui_win_redraw(ctx->win); } return; }
    if (key == ARPILE_KEY_DOWN) { st->scroll++; arpile_ui_win_redraw(ctx->win); return; }
    if (key == ARPILE_KEY_F7) { st->show_help = !st->show_help; arpile_ui_win_redraw(ctx->win); return; }
}

static const arpile_app_ops_t phys_ops = {
    .init = phys_init,
    .event = phys_event,
    .render = phys_render,
    .destroy = phys_destroy,
};

const arpile_app_t arpile_app_phys = {
    .id = "phys",
    .name = "Physics",
    .icon = "phys",
    .ops = &phys_ops,
};