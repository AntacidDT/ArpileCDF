#include "arpile_ui.h"
#include "arpile_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIO_MAX_SECTORS 12
#define BIO_MAX_PAGES 5
#define BIO_MAX_LINE 128

typedef struct {
    const char *title;
    const char *lines[BIO_MAX_PAGES];
    int page_count;
} bio_page_t;

typedef struct {
    const char *name;
    bio_page_t pages[BIO_MAX_PAGES];
    int page_cnt;
} bio_sector_t;

static const bio_sector_t g_sectors[] = {
    {
        "Blood",
        {
            {"Blood Groups", {"ABO system defines four blood types: A, B, AB, O based on antigens on red cells.", "Rh factor (+/-) adds another antigen; Rh- can receive only Rh- blood.", "Compatibility: O- is universal donor; AB+ universal recipient."}, 3},
            {"Blood Types", {"Type A has A antigens, anti-B antibodies.", "Type B has B antigens, anti-A antibodies.", "Type AB has both antigens, no anti-A/B.", "Type O has no antigens, both anti-A and anti-B."}, 4},
            {"Blood Components", {"Red cells carry O2; White cells fight infection; Platelets clot.", "Plasma is the liquid carrier (water, proteins, electrolytes)."}, 2},
            {"Blood Compatibility", {"Transfusion must match ABO and Rh.", "Cross‑matching prevents hemolytic reactions."}, 2},
        },
        4
    },
    {
        "Vaccines",
        {
            {"What Are Vaccines", {"Vaccines train the immune system using harmless antigen.", "They create memory cells for rapid response on real infection."}, 2},
            {"Types of Vaccines", {"Live‑attenuated (measles, mumps, rubella).", "Inactivated (polio, flu).", "Subunit / conjugate (HPV, pneumococcal).", "mRNA (COVID‑19)."}, 4},
        },
        2
    },
    {
        "Cells",
        {
            {"Cell Basics", {"Cells are the basic unit of life.", "Prokaryotes lack nucleus (bacteria).", "Eukaryotes have nucleus and organelles (plants, animals)."}, 3},
            {"Organelles", {"Nucleus stores DNA; Mitochondria make ATP.", "Ribosomes make proteins; ER & Golgi process/transport.", "Lysosomes digest waste; Chloroplasts (plants) photosynthesize."}, 4},
        },
        2
    },
    {
        "DNA",
        {
            {"DNA Structure", {"Double helix of two strands.", "Four bases: A pairs T, C pairs G.", "Sugar‑phosphate backbone."}, 3},
            {"Replication & Genes", {"Semi‑conservative replication each division.", "Gene = segment coding a protein.", "Transcription -> mRNA -> Translation -> protein."}, 3},
        },
        2
    },
    {
        "Anatomy",
        {
            {"Body Systems Overview", {"Circulatory: heart, blood, vessels.", "Respiratory: lungs, airways.", "Digestive: mouth→stomach→intestines.", "Nervous: brain, spinal cord, nerves.", "Musculoskeletal: bones, muscles."}, 5},
        },
        1
    },
    {
        "Genetics",
        {
            {"Mendelian Genetics", {"Dominant allele masks recessive.", "Homozygous vs heterozygous.", "Punnett square predicts ratios."}, 3},
        },
        1
    },
    {
        "Microbiology",
        {
            {"Bacteria vs Viruses", {"Bacteria: single‑cell, can live independently.", "Viruses: need host cell to replicate.", "Antibiotics kill bacteria, not viruses."}, 3},
        },
        1
    },
    {
        "Evolution",
        {
            {"Natural Selection", {"Variation exists in populations.", "Traits improving survival reproduce more.", "Over generations, populations adapt."}, 3},
        },
        1
    },
    {
        "Diseases",
        {
            {"Infectious Diseases", {"Bacterial (TB, strep).", "Viral (flu, HIV).", "Fungal, parasitic."}, 3},
            {"Non‑communicable", {"Cardiovascular, cancer, diabetes, chronic respiratory."}, 1},
        },
        2
    },
    {
        "Disorders",
        {
            {"Genetic Disorders", {"Cystic fibrosis (CFTR mutation).", "Sickle cell anemia (hemoglobin S).", "Down syndrome (trisomy 21)."}, 3},
        },
        1
    },
    {
        "Symptoms",
        {
            {"Common Signs", {"Fever = immune response.", "Pain = tissue damage signal.", "Fatigue = many causes.", "Swelling = inflammation."}, 4},
        },
        1
    },
    {
        "Taxonomy",
        {
            {"Classification Ranks", {"Domain, Kingdom, Phylum, Class, Order, Family, Genus, Species.", "Binomial nomenclature: Genus species (e.g., Homo sapiens)."}, 2},
        },
        1
    },
};
#define BIO_SECTOR_COUNT (sizeof(g_sectors)/sizeof(g_sectors[0]))

typedef struct {
    int sector;
    int page;
    int scroll;
    bool show_help;
} bio_state_t;

static void bio_draw_sectors(ili9488_t *lcd, const ui_rect_t *r, int cur, int scroll) {
    ui_draw_fill_rect(lcd, r, UI_C_WIN_BG);
    ui_draw_outline(lcd, r, UI_C_BORDER);
    ui_draw_text(lcd, r->x + 4, r->y + 2, "Sectors", UI_C_TEXT_DIM, UI_C_WIN_BG);
    int line_h = CHAR_H + 2;
    int max = (r->h - 20) / line_h;
    int start = scroll;
    int end = start + max;
    if (end > BIO_SECTOR_COUNT) { end = BIO_SECTOR_COUNT; start = end - max; if (start < 0) start = 0; }
    int y = r->y + 18;
    for (int i = start; i < end; i++) {
        uint16_t col = (i == cur) ? UI_C_ACCENT : UI_C_TEXT;
        ui_draw_text(lcd, r->x + 4, y, g_sectors[i].name, col, UI_C_WIN_BG);
        y += CHAR_H + 2;
    }
}

static void draw_wrapped_line(ili9488_t *lcd, int x, int *y, const char *text, uint16_t color, int max_chars, int line_h, int bottom_limit) {
    const char *p = text;
    char line[128];
    int line_len = 0;
    while (*p) {
        // skip spaces
        while (*p == ' ') p++;
        const char *word_start = p;
        int word_len = 0;
        while (*p && *p != ' ') { p++; word_len++; }
        if (word_len == 0) break;
        if (line_len + word_len + (line_len?1:0) > max_chars) {
            if (line_len > 0) {
                line[line_len] = 0;
                if (*y + CHAR_H > bottom_limit) return;
                ui_draw_text(lcd, x, *y, line, color, UI_C_WIN_BG);
                *y += CHAR_H + 2;
                line_len = 0;
            }
        }
        if (line_len > 0) { line[line_len++] = ' '; }
        if (word_len > max_chars) {
            // word too long, split
            for (int i = 0; i < word_len; i += max_chars) {
                int chunk = word_len - i;
                if (chunk > max_chars) chunk = max_chars;
                memcpy(line, word_start + i, chunk);
                line[chunk] = 0;
                if (*y + CHAR_H > bottom_limit) return;
                ui_draw_text(lcd, x, *y, line, color, UI_C_WIN_BG);
                *y += CHAR_H + 2;
            }
        } else {
            memcpy(line + line_len, word_start, word_len);
            line_len += word_len;
        }
    }
    if (line_len > 0) {
        line[line_len] = 0;
        if (*y + CHAR_H <= bottom_limit) {
            ui_draw_text(lcd, x, *y, line, color, UI_C_WIN_BG);
            *y += CHAR_H + 2;
        }
    }
}

static void bio_draw_content(ili9488_t *lcd, const ui_rect_t *r, const bio_sector_t *sec, int page, int scroll) {
    ui_draw_fill_rect(lcd, r, UI_C_WIN_BG);
    ui_draw_outline(lcd, r, UI_C_BORDER);
    if (page >= sec->page_cnt) page = sec->page_cnt - 1;
    const bio_page_t *pg = &sec->pages[page];
    int y = r->y + 4;
    int max_chars = (r->w - 12) / CHAR_W;
    if (max_chars < 10) max_chars = 10;
    int bottom_limit = r->y + r->h - 4 - (CHAR_H + 2); // leave space for page indicator

    // header
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "%s   %d/%d", pg->title, page+1, pg->page_count);
    ui_draw_text(lcd, r->x + 4, y, hdr, UI_C_TEXT, UI_C_WIN_BG);
    y += CHAR_H + 4;

    // draw wrapped lines from all page lines sequentially
    for (int i = 0; i < pg->page_count; i++) {
        if (y + CHAR_H > bottom_limit) break;
        draw_wrapped_line(lcd, r->x + 4, &y, pg->lines[i], UI_C_TEXT, max_chars, CHAR_H + 2, bottom_limit);
    }

    // page indicator at bottom
    char pinfo[32];
    snprintf(pinfo, sizeof(pinfo), "PgUp/PgDn  %d/%d", page+1, sec->page_cnt);
    ui_draw_text(lcd, r->x + 4, r->y + r->h - (CHAR_H + 2) - 2, pinfo, UI_C_TEXT_DIM, UI_C_WIN_BG);
}

static void bio_draw_help(ili9488_t *lcd, const ui_rect_t *r) {
    ui_draw_fill_rect(lcd, r, UI_C_PANEL);
    ui_draw_outline(lcd, r, UI_C_ACCENT);
    const char *lines[] = {
        "Biology Shortcuts:",
        "  Tab / Shift+Tab   next / prev sector",
        "  PgDn / PgUp       next / prev page",
        "  Enter             (reserved)",
        "  Esc               close help / back",
        "  F7                toggle this help",
        "  F10               fullscreen",
        "  Mouse wheel       scroll article",
        "  Click sector      select",
    };
    int y = (int)r->y + 6;
    for (size_t i = 0; i < sizeof(lines)/sizeof(lines[0]); i++) {
        int line_y = y + (int)i * (CHAR_H + 2);
        ui_draw_text(lcd, r->x + 8, (uint16_t)line_y, lines[i], UI_C_TEXT, UI_C_PANEL);
    }
}

static void bio_init(arpile_app_ctx_t *ctx) {
    bio_state_t *st = malloc(sizeof(bio_state_t));
    if (!st) return;
    memset(st, 0, sizeof(bio_state_t));
    ctx->user = st;
    if (ctx->win) ctx->win->state.capture_nav = false;
}

static void bio_destroy(arpile_app_ctx_t *ctx) {
    bio_state_t *st = ctx->user;
    free(st);
    ctx->user = NULL;
}

static void bio_render(arpile_app_ctx_t *ctx, ui_win_t *win) {
    bio_state_t *st = ctx->user;
    if (!st) { bio_init(ctx); st = ctx->user; if (!st) return; }

    ili9488_t *lcd = arpile_ui_get_lcd();
    ui_rect_t c = win_client_rect(win);
    ui_draw_fill_rect(lcd, &c, UI_C_WIN_BG);

    ui_rect_t left = { c.x, c.y, 180, c.h };
    ui_rect_t right = { (uint16_t)(c.x + 180), c.y, (uint16_t)(c.w - 180), c.h };

    bio_draw_sectors(lcd, &left, st->sector, 0);
    bio_draw_content(lcd, &right, &g_sectors[st->sector], st->page, st->scroll);

    if (st->show_help) {
        ui_rect_t hp = { c.x + 40, c.y + 30, 240, 200 };
        bio_draw_help(lcd, &hp);
    }
}

static void bio_event(arpile_app_ctx_t *ctx, const arpile_input_event_t *ev) {
    bio_state_t *st = ctx->user;
    if (!st) return;
    if (ev->type != ARPILE_IN_EVENT_KEY_DOWN) return;
    uint16_t key = ev->key.keycode;
    uint8_t mod = ev->key.modifier;
    bool shift = mod & (ARPILE_MOD_LSHIFT|ARPILE_MOD_RSHIFT);

    if (key == ARPILE_KEY_F7) {
        st->show_help = !st->show_help;
        arpile_ui_win_redraw(ctx->win);
        return;
    }
    if (key == ARPILE_KEY_F10) return; // WM handles
    if (key == ARPILE_KEY_ESCAPE) {
        if (st->show_help) { st->show_help = false; arpile_ui_win_redraw(ctx->win); return; }
    }

    if (key == ARPILE_KEY_TAB) {
        if (shift) {
            st->sector = (st->sector - 1 + BIO_SECTOR_COUNT) % BIO_SECTOR_COUNT;
        } else {
            st->sector = (st->sector + 1) % BIO_SECTOR_COUNT;
        }
        st->page = 0; st->scroll = 0;
        arpile_ui_win_redraw(ctx->win);
        return;
    }
    if (key == ARPILE_KEY_PGDN) {
        if (st->page + 1 < g_sectors[st->sector].page_cnt) {
            st->page++; st->scroll = 0; arpile_ui_win_redraw(ctx->win);
        }
        return;
    }
    if (key == ARPILE_KEY_PGUP) {
        if (st->page > 0) { st->page--; st->scroll = 0; arpile_ui_win_redraw(ctx->win); }
        return;
    }
    if (key == ARPILE_KEY_UP) {
        if (st->scroll > 0) { st->scroll--; arpile_ui_win_redraw(ctx->win); }
        return;
    }
    if (key == ARPILE_KEY_DOWN) {
        // simple scroll down
        st->scroll++; arpile_ui_win_redraw(ctx->win);
        return;
    }
    if (key == ARPILE_KEY_F7) {
        st->show_help = !st->show_help;
        arpile_ui_win_redraw(ctx->win);
        return;
    }
    if (key == ARPILE_KEY_F10) return; // WM
}

static const arpile_app_ops_t bio_ops = {
    .init = bio_init,
    .event = bio_event,
    .render = bio_render,
    .destroy = bio_destroy,
};

const arpile_app_t arpile_app_bio = {
    .id = "bio",
    .name = "Biology",
    .icon = "bio",
    .ops = &bio_ops,
};