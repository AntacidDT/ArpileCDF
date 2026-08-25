#include "arpile_ui.h"
#include "arpile_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#pragma GCC diagnostic pop

#define CHEM_MAX_ELEMENTS 118
#define CHEM_INPUT_MAX 128
#define CHEM_PTABLE_COLS 18
#define CHEM_PTABLE_ROWS 7

/* 5x7 font (ASCII 32‑126). Each char = 5 columns, each column 7 bits (LSB = top). */
static const uint8_t font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x14,0x08,0x3E,0x08,0x14}, /* * */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x08,0x14,0x22,0x41,0x00}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x00,0x41,0x22,0x14,0x08}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x7F,0x20,0x18,0x20,0x7F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x00,0x7F,0x41,0x41,0x00}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */
    {0x00,0x41,0x41,0x7F,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* _ */
    {0x00,0x01,0x02,0x04,0x00}, /* ` */
    {0x20,0x54,0x54,0x54,0x78}, /* a */
    {0x7F,0x48,0x44,0x44,0x38}, /* b */
    {0x38,0x44,0x44,0x44,0x28}, /* c */
    {0x38,0x44,0x44,0x48,0x7F}, /* d */
    {0x38,0x54,0x54,0x54,0x18}, /* e */
    {0x08,0x7E,0x09,0x01,0x02}, /* f */
    {0x0C,0x52,0x52,0x52,0x3E}, /* g */
    {0x7F,0x08,0x04,0x04,0x78}, /* h */
    {0x00,0x44,0x7D,0x40,0x00}, /* i */
    {0x20,0x40,0x44,0x3D,0x00}, /* j */
    {0x7F,0x10,0x28,0x44,0x00}, /* k */
    {0x00,0x41,0x7F,0x40,0x00}, /* l */
    {0x7C,0x04,0x18,0x04,0x78}, /* m */
    {0x7C,0x08,0x04,0x04,0x78}, /* n */
    {0x38,0x44,0x44,0x44,0x38}, /* o */
    {0x7C,0x14,0x14,0x14,0x08}, /* p */
    {0x08,0x14,0x14,0x18,0x7C}, /* q */
    {0x7C,0x08,0x04,0x04,0x08}, /* r */
    {0x48,0x54,0x54,0x54,0x20}, /* s */
    {0x04,0x3F,0x44,0x40,0x20}, /* t */
    {0x3C,0x40,0x40,0x20,0x7C}, /* u */
    {0x1C,0x20,0x40,0x20,0x1C}, /* v */
    {0x3C,0x40,0x30,0x40,0x3C}, /* w */
    {0x44,0x28,0x10,0x28,0x44}, /* x */
    {0x0C,0x50,0x50,0x50,0x3C}, /* y */
    {0x44,0x64,0x54,0x4C,0x44}, /* z */
    {0x00,0x08,0x36,0x41,0x00}, /* { */
    {0x00,0x00,0x7F,0x00,0x00}, /* | */
    {0x00,0x41,0x36,0x08,0x00}, /* } */
    {0x10,0x08,0x08,0x10,0x08}, /* ~ */
};

/* small‑font helpers ------------------------------------------------*/
#define SF_W 5
#define SF_H 7
#define SF_FIRST 32
#define SF_LAST  126

static void draw_small_char(ili9488_t *lcd, int x, int y, char ch,
                             uint16_t fg, uint16_t bg)
{
    if (ch < SF_FIRST || ch > SF_LAST) ch = '?';
    const uint8_t *col = font5x7[ch - SF_FIRST];
    for (int cx = 0; cx < SF_W; ++cx) {
        uint8_t bits = col[cx];
        for (int row = 0; row < SF_H; ++row) {
            if (bits & (1 << row))
                ui_draw_fill_rect(lcd,
                    &(ui_rect_t){ (uint16_t)(x + cx), (uint16_t)(y + row), 1, 1 }, fg);
            else
                ui_draw_fill_rect(lcd,
                    &(ui_rect_t){ (uint16_t)(x + cx), (uint16_t)(y + row), 1, 1 }, bg);
        }
    }
}

static void draw_small_text(ili9488_t *lcd, int x, int y, const char *s,
                             uint16_t fg, uint16_t bg)
{
    int cx = x;
    while (*s) {
        draw_small_char(lcd, cx, y, *s, fg, bg);
        cx += SF_W + 1;          /* 1‑pixel spacing */
        ++s;
    }
}

/* small‑text wrap ----------------------------------------------------*/
static void draw_wrapped_small(ili9488_t *lcd, int x, int *y, const char *text,
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
                if (*y + SF_H > bottom_limit) return;
                draw_small_text(lcd, x, *y, line, UI_C_TEXT, UI_C_WIN_BG);
                *y += SF_H + 1;
                line_len = 0;
            }
        }
        if (line_len) line[line_len++] = ' ';
        if (wl > max_chars) {
            for (int i=0;i<wl;i+=max_chars) {
                int chunk = wl - i; if (chunk > max_chars) chunk = max_chars;
                memcpy(line, ws+i, chunk); line[chunk]=0;
                if (*y + SF_H > bottom_limit) return;
                draw_small_text(lcd, x, *y, line, UI_C_TEXT, UI_C_WIN_BG);
                *y += SF_H + 1;
            }
        } else {
            memcpy(line+line_len, ws, wl);
            line_len += wl;
        }
    }
    if (line_len) {
        line[line_len]=0;
        if (*y + SF_H <= bottom_limit) {
            draw_small_text(lcd, x, *y, line, UI_C_TEXT, UI_C_WIN_BG);
            *y += SF_H + 1;
        }
    }
}

/* category colours (RGB565) */
static const uint16_t CAT_COL_ALKALI        = UI_RGB(0xE0,0x40,0x40);   /* red */
static const uint16_t CAT_COL_ALKALINE_EARTH = UI_RGB(0xE0,0xA0,0x40);   /* orange */
static const uint16_t CAT_COL_TRANSITION    = UI_RGB(0x40,0xA0,0xE0);   /* blue */
static const uint16_t CAT_COL_POST_TRANSITION = UI_RGB(0xA0,0x40,0xE0); /* purple */
static const uint16_t CAT_COL_METALLOID     = UI_RGB(0xE0,0xE0,0x40);   /* yellow */
static const uint16_t CAT_COL_NONMETAL      = UI_RGB(0x40,0xE0,0x40);   /* green */
static const uint16_t CAT_COL_HALOGEN       = UI_RGB(0xE0,0x40,0xA0);   /* pink */
static const uint16_t CAT_COL_NOBLE_GAS     = UI_RGB(0xA0,0xA0,0xA0);   /* gray */
static const uint16_t CAT_COL_LANTHANIDE    = UI_RGB(0xFF,0x80,0x00);   /* orange */
static const uint16_t CAT_COL_ACTINIDE      = UI_RGB(0xFF,0x00,0x80);   /* magenta */

static uint16_t cat_color(const char *cat)
{
    if (!cat) return UI_C_WIN_BG;
    if (!strcmp(cat, "Alkali metal")) return CAT_COL_ALKALI;
    if (!strcmp(cat, "Alkaline earth")) return CAT_COL_ALKALINE_EARTH;
    if (!strcmp(cat, "Transition")) return CAT_COL_TRANSITION;
    if (!strcmp(cat, "Post-transition")) return CAT_COL_POST_TRANSITION;
    if (!strcmp(cat, "Metalloid")) return CAT_COL_METALLOID;
    if (!strcmp(cat, "Nonmetal")) return CAT_COL_NONMETAL;
    if (!strcmp(cat, "Halogen")) return CAT_COL_HALOGEN;
    if (!strcmp(cat, "Noble gas")) return CAT_COL_NOBLE_GAS;
    if (!strcmp(cat, "Lanthanide")) return CAT_COL_LANTHANIDE;
    if (!strcmp(cat, "Actinide")) return CAT_COL_ACTINIDE;
    return UI_C_WIN_BG;
}

typedef enum {
    CHEM_TOOL_PERIODIC = 0,
    CHEM_TOOL_MOLAR_MASS,
    CHEM_TOOL_PH,
    CHEM_TOOL_GAS_LAWS,
    CHEM_TOOL_STOICHIOMETRY,
    CHEM_TOOL_THERMOCHEMISTRY,
    CHEM_TOOL_ORGANIC,
    CHEM_TOOL_REDOX,
    CHEM_TOOL_BONDING,
    CHEM_TOOL_ELECTRON_CONFIG,
    CHEM_TOOL_EQUATION_BALANCE,
    CHEM_TOOL_TITRATION,
    CHEM_TOOL_COUNT
} chem_tool_t;

typedef struct {
    int atomic_num;
    const char *symbol;
    const char *name;
    double mass;
    int group;
    int period;
    const char *block;
    const char *electron_config;
    const char *oxidation_states;
    const char *state;
    const char *category;
    double electronegativity;
    double melting_point;
    double boiling_point;
    double density;
    int ptable_x;
    int ptable_y;
} chem_element_t;

static const chem_element_t g_elements[CHEM_MAX_ELEMENTS] = {
    {1, "H", "Hydrogen", 1.008, 1, 1, "s", "1s1", "+1", "Gas", "Nonmetal", 2.20, -259.16, -252.87, 0.0000899, 0, 0},
    {2, "He", "Helium", 4.003, 18, 1, "s", "1s2", "0", "Gas", "Noble gas", 0, -272.20, -268.93, 0.000178, 17, 0},
    {3, "Li", "Lithium", 6.94, 1, 2, "s", "[He] 2s1", "+1", "Solid", "Alkali metal", 0.98, 180.54, 1342, 0.534, 0, 1},
    {4, "Be", "Beryllium", 9.012, 2, 2, "s", "[He] 2s2", "+2", "Solid", "Alkaline earth", 1.57, 1287, 2470, 1.85, 1, 1},
    {5, "B", "Boron", 10.81, 13, 2, "p", "[He] 2s2 2p1", "+3", "Solid", "Metalloid", 2.04, 2075, 4000, 2.34, 12, 1},
    {6, "C", "Carbon", 12.011, 14, 2, "p", "[He] 2s2 2p2", "+4,+2,-4", "Solid", "Nonmetal", 2.55, 3550, 4027, 2.267, 13, 1},
    {7, "N", "Nitrogen", 14.007, 15, 2, "p", "[He] 2s2 2p3", "+5,+3,-3", "Gas", "Nonmetal", 3.04, -210.00, -195.79, 0.001251, 14, 1},
    {8, "O", "Oxygen", 15.999, 16, 2, "p", "[He] 2s2 2p4", "-2", "Gas", "Nonmetal", 3.44, -218.79, -182.95, 0.001429, 15, 1},
    {9, "F", "Fluorine", 18.998, 17, 2, "p", "[He] 2s2 2p5", "-1", "Gas", "Halogen", 3.98, -219.67, -188.11, 0.001696, 16, 1},
    {10, "Ne", "Neon", 20.180, 18, 2, "p", "[He] 2s2 2p6", "0", "Gas", "Noble gas", 0, -248.59, -246.05, 0.000900, 17, 1},
    {11, "Na", "Sodium", 22.990, 1, 3, "s", "[Ne] 3s1", "+1", "Solid", "Alkali metal", 0.93, 97.79, 883, 0.97, 0, 2},
    {12, "Mg", "Magnesium", 24.305, 2, 3, "s", "[Ne] 3s2", "+2", "Solid", "Alkaline earth", 1.31, 650, 1090, 1.74, 1, 2},
    {13, "Al", "Aluminum", 26.982, 13, 3, "p", "[Ne] 3s2 3p1", "+3", "Solid", "Post-transition", 1.61, 660.32, 2519, 2.70, 12, 2},
    {14, "Si", "Silicon", 28.085, 14, 3, "p", "[Ne] 3s2 3p2", "+4, -4", "Solid", "Metalloid", 1.90, 1414, 3265, 2.33, 13, 2},
    {15, "P", "Phosphorus", 30.974, 15, 3, "p", "[Ne] 3s2 3p3", "+5,+3,-3", "Solid", "Nonmetal", 2.19, 44.15, 280.5, 1.82, 14, 2},
    {16, "S", "Sulfur", 32.06, 16, 3, "p", "[Ne] 3s2 3p4", "+6,+4,-2", "Solid", "Nonmetal", 2.58, 115.21, 444.6, 2.07, 15, 2},
    {17, "Cl", "Chlorine", 35.45, 17, 3, "p", "[Ne] 3s2 3p5", "+7,+5,+1,-1", "Gas", "Halogen", 3.16, -101.5, -34.04, 0.0032, 16, 2},
    {18, "Ar", "Argon", 39.95, 18, 3, "p", "[Ne] 3s2 3p6", "0", "Gas", "Noble gas", 0, -189.35, -185.85, 0.00178, 17, 2},
    {19, "K", "Potassium", 39.10, 1, 4, "s", "[Ar] 4s1", "+1", "Solid", "Alkali metal", 0.82, 63.5, 759, 0.86, 0, 3},
    {20, "Ca", "Calcium", 40.08, 2, 4, "s", "[Ar] 4s2", "+2", "Solid", "Alkaline earth", 1.00, 842, 1484, 1.55, 1, 3},
    {21, "Sc", "Scandium", 44.96, 3, 4, "d", "[Ar] 3d1 4s2", "+3", "Solid", "Transition", 1.36, 1541, 2836, 2.99, 2, 3},
    {22, "Ti", "Titanium", 47.87, 4, 4, "d", "[Ar] 3d2 4s2", "+4", "Solid", "Transition", 1.54, 1668, 3287, 4.51, 3, 3},
    {23, "V", "Vanadium", 50.94, 5, 4, "d", "[Ar] 3d3 4s2", "+5,+3,+2", "Solid", "Transition", 1.63, 1910, 3407, 6.0, 4, 3},
    {24, "Cr", "Chromium", 52.00, 6, 4, "d", "[Ar] 3d5 4s1", "+6,+3,+2", "Solid", "Transition", 1.66, 1907, 2671, 7.19, 5, 3},
    {25, "Mn", "Manganese", 54.94, 7, 4, "d", "[Ar] 3d5 4s2", "+7,+4,+2", "Solid", "Transition", 1.55, 1246, 2061, 7.21, 6, 3},
    {26, "Fe", "Iron", 55.85, 8, 4, "d", "[Ar] 3d6 4s2", "+3,+2", "Solid", "Transition", 1.83, 1538, 2862, 7.87, 7, 3},
    {27, "Co", "Cobalt", 58.93, 9, 4, "d", "[Ar] 3d7 4s2", "+3,+2", "Solid", "Transition", 1.88, 1495, 2927, 8.9, 8, 3},
    {28, "Ni", "Nickel", 58.69, 10, 4, "d", "[Ar] 3d8 4s2", "+2", "Solid", "Transition", 1.91, 1455, 2913, 8.91, 9, 3},
    {29, "Cu", "Copper", 63.55, 11, 4, "d", "[Ar] 3d10 4s1", "+2,+1", "Solid", "Transition", 1.90, 1085, 2562, 8.96, 10, 3},
    {30, "Zn", "Zinc", 65.38, 12, 4, "d", "[Ar] 3d10 4s2", "+2", "Solid", "Transition", 1.65, 419.5, 907, 7.14, 11, 3},
    {31, "Ga", "Gallium", 69.72, 13, 4, "p", "[Ar] 3d10 4s2 4p1", "+3", "Solid", "Post-transition", 1.81, 29.76, 2204, 5.91, 12, 3},
    {32, "Ge", "Germanium", 72.63, 14, 4, "p", "[Ar] 3d10 4s2 4p2", "+4,+2", "Solid", "Metalloid", 2.01, 938.25, 2833, 5.35, 13, 3},
    {33, "As", "Arsenic", 74.92, 15, 4, "p", "[Ar] 3d10 4s2 4p3", "+5,+3,-3", "Solid", "Metalloid", 2.18, 817, 614, 5.73, 14, 3},
    {34, "Se", "Selenium", 78.97, 16, 4, "p", "[Ar] 3d10 4s2 4p4", "+6,+4,-2", "Solid", "Nonmetal", 2.55, 221, 685, 4.81, 15, 3},
    {35, "Br", "Bromine", 79.90, 17, 4, "p", "[Ar] 3d10 4s2 4p5", "+5,+1,-1", "Liquid", "Halogen", 2.96, -7.2, 58.8, 3.12, 16, 3},
    {36, "Kr", "Krypton", 83.80, 18, 4, "p", "[Ar] 3d10 4s2 4p6", "0", "Gas", "Noble gas", 3.00, -157.37, -153.22, 0.00375, 17, 3},
    {37, "Rb", "Rubidium", 85.47, 1, 5, "s", "[Kr] 5s1", "+1", "Solid", "Alkali metal", 0.82, 39.31, 688, 1.53, 0, 4},
    {38, "Sr", "Strontium", 87.62, 2, 5, "s", "[Kr] 5s2", "+2", "Solid", "Alkaline earth", 0.95, 777, 1382, 2.64, 1, 4},
    {39, "Y", "Yttrium", 88.91, 3, 5, "d", "[Kr] 4d1 5s2", "+3", "Solid", "Transition", 1.22, 1522, 3345, 4.47, 2, 4},
    {40, "Zr", "Zirconium", 91.22, 4, 5, "d", "[Kr] 4d2 5s2", "+4", "Solid", "Transition", 1.33, 1855, 4409, 6.52, 3, 4},
    {41, "Nb", "Niobium", 92.91, 5, 5, "d", "[Kr] 4d4 5s1", "+5", "Solid", "Transition", 1.6, 2477, 4744, 8.57, 4, 4},
    {42, "Mo", "Molybdenum", 95.95, 6, 5, "d", "[Kr] 4d5 5s1", "+6", "Solid", "Transition", 2.16, 2623, 4639, 10.2, 5, 4},
    {43, "Tc", "Technetium", 98.0, 7, 5, "d", "[Kr] 4d5 5s2", "+7", "Solid", "Transition", 1.9, 2157, 4265, 11.5, 6, 4},
    {44, "Ru", "Ruthenium", 101.1, 8, 5, "d", "[Kr] 4d7 5s1", "+8,+3", "Solid", "Transition", 2.2, 2334, 4150, 12.4, 7, 4},
    {45, "Rh", "Rhodium", 102.9, 9, 5, "d", "[Kr] 4d8 5s1", "+3", "Solid", "Transition", 2.28, 1964, 3695, 12.4, 8, 4},
    {46, "Pd", "Palladium", 106.4, 10, 5, "d", "[Kr] 4d10", "+2,+4", "Solid", "Transition", 2.20, 1554.9, 2963, 12.0, 9, 4},
    {47, "Ag", "Silver", 107.9, 11, 5, "d", "[Kr] 4d10 5s1", "+1", "Solid", "Transition", 1.93, 961.78, 2162, 10.5, 10, 4},
    {48, "Cd", "Cadmium", 112.4, 12, 5, "d", "[Kr] 4d10 5s2", "+2", "Solid", "Transition", 1.69, 321.07, 767, 8.65, 11, 4},
    {49, "In", "Indium", 114.8, 13, 5, "p", "[Kr] 4d10 5s2 5p1", "+3", "Solid", "Post-transition", 1.78, 156.6, 2072, 7.31, 12, 4},
    {50, "Sn", "Tin", 118.7, 14, 5, "p", "[Kr] 4d10 5s2 5p2", "+4,+2", "Solid", "Post-transition", 1.96, 231.93, 2602, 7.31, 13, 4},
    {51, "Sb", "Antimony", 121.8, 15, 5, "p", "[Kr] 4d10 5s2 5p3", "+5,+3,-3", "Solid", "Metalloid", 2.05, 630.63, 1587, 6.7, 14, 4},
    {52, "Te", "Tellurium", 127.6, 16, 5, "p", "[Kr] 4d10 5s2 5p4", "+6,+4,-2", "Solid", "Metalloid", 2.1, 449.51, 988, 6.24, 15, 4},
    {53, "I", "Iodine", 126.9, 17, 5, "p", "[Kr] 4d10 5s2 5p5", "+7,+5,+1,-1", "Solid", "Halogen", 2.66, 113.7, 184.3, 4.93, 16, 4},
    {54, "Xe", "Xenon", 131.3, 18, 5, "p", "[Kr] 4d10 5s2 5p6", "0", "Gas", "Noble gas", 2.6, -111.75, -108.1, 0.0059, 17, 4},
    {55, "Cs", "Cesium", 132.9, 1, 6, "s", "[Xe] 6s1", "+1", "Solid", "Alkali metal", 0.79, 28.44, 671, 1.93, 0, 5},
    {56, "Ba", "Barium", 137.3, 2, 6, "s", "[Xe] 6s2", "+2", "Solid", "Alkaline earth", 0.89, 727, 1845, 3.51, 1, 5},
    {57, "La", "Lanthanum", 138.9, 3, 6, "f", "[Xe] 5d1 6s2", "+3", "Solid", "Lanthanide", 1.10, 920, 3464, 6.15, 2, 5},
    {58, "Ce", "Cerium", 140.1, 3, 6, "f", "[Xe] 4f1 5d1 6s2", "+4,+3", "Solid", "Lanthanide", 1.12, 798, 3443, 6.77, 3, 5},
    {59, "Pr", "Praseodymium", 140.9, 3, 6, "f", "[Xe] 4f3 6s2", "+3", "Solid", "Lanthanide", 1.13, 931, 3520, 6.77, 4, 5},
    {60, "Nd", "Neodymium", 144.2, 3, 6, "f", "[Xe] 4f4 6s2", "+3", "Solid", "Lanthanide", 1.14, 1021, 3074, 7.01, 5, 5},
    {61, "Pm", "Promethium", 145.0, 3, 6, "f", "[Xe] 4f5 6s2", "+3", "Solid", "Lanthanide", 1.13, 1042, 3000, 7.26, 6, 5},
    {62, "Sm", "Samarium", 150.4, 3, 6, "f", "[Xe] 4f6 6s2", "+3,+2", "Solid", "Lanthanide", 1.17, 1072, 1794, 7.52, 7, 5},
    {63, "Eu", "Europium", 152.0, 3, 6, "f", "[Xe] 4f7 6s2", "+3,+2", "Solid", "Lanthanide", 1.2, 822, 1529, 5.24, 8, 5},
    {64, "Gd", "Gadolinium", 157.3, 3, 6, "f", "[Xe] 4f7 5d1 6s2", "+3", "Solid", "Lanthanide", 1.20, 1313, 3273, 7.90, 9, 5},
    {65, "Tb", "Terbium", 158.9, 3, 6, "f", "[Xe] 4f9 6s2", "+3,+4", "Solid", "Lanthanide", 1.1, 1356, 3230, 8.23, 10, 5},
    {66, "Dy", "Dysprosium", 162.5, 3, 6, "f", "[Xe] 4f10 6s2", "+3", "Solid", "Lanthanide", 1.22, 1412, 2567, 8.55, 11, 5},
    {67, "Ho", "Holmium", 164.9, 3, 6, "f", "[Xe] 4f11 6s2", "+3", "Solid", "Lanthanide", 1.23, 1472, 2700, 8.80, 12, 5},
    {67, "Er", "Erbium", 167.3, 3, 6, "f", "[Xe] 4f12 6s2", "+3", "Solid", "Lanthanide", 1.24, 1529, 2868, 9.07, 13, 5},
    {68, "Tm", "Thulium", 168.9, 3, 6, "f", "[Xe] 4f13 6s2", "+3", "Solid", "Lanthanide", 1.25, 1545, 1950, 9.32, 14, 5},
    {69, "Yb", "Ytterbium", 173.0, 3, 6, "f", "[Xe] 4f14 6s2", "+3,+2", "Solid", "Lanthanide", 1.1, 819, 1196, 6.90, 15, 5},
    {70, "Lu", "Lutetium", 175.0, 3, 6, "d", "[Xe] 4f14 5d1 6s2", "+3", "Solid", "Lanthanide", 1.27, 1663, 3402, 9.84, 16, 5},
    {71, "Hf", "Hafnium", 178.5, 4, 6, "d", "[Xe] 4f14 5d2 6s2", "+4", "Solid", "Transition", 1.3, 2233, 4603, 13.3, 3, 5},
    {72, "Ta", "Tantalum", 180.9, 5, 6, "d", "[Xe] 4f14 5d3 6s2", "+5", "Solid", "Transition", 1.5, 3017, 5458, 16.6, 4, 5},
    {73, "W", "Tungsten", 183.8, 6, 6, "d", "[Xe] 4f14 5d4 6s2", "+6", "Solid", "Transition", 2.36, 3422, 5555, 19.3, 5, 5},
    {74, "Re", "Rhenium", 186.2, 7, 6, "d", "[Xe] 4f14 5d5 6s2", "+7", "Solid", "Transition", 1.9, 3186, 5596, 21.0, 6, 5},
    {75, "Os", "Osmium", 190.2, 8, 6, "d", "[Xe] 4f14 5d6 6s2", "+8,+4", "Solid", "Transition", 2.2, 3033, 5012, 22.6, 7, 5},
    {76, "Ir", "Iridium", 192.2, 9, 6, "d", "[Xe] 4f14 5d7 6s2", "+4,+3", "Solid", "Transition", 2.20, 2410, 4130, 22.6, 8, 5},
    {77, "Pt", "Platinum", 195.1, 10, 6, "d", "[Xe] 4f14 5d9 6s1", "+4,+2", "Solid", "Transition", 2.28, 1768, 3825, 21.5, 9, 5},
    {78, "Au", "Gold", 197.0, 11, 6, "d", "[Xe] 4f14 5d10 6s1", "+3,+1", "Solid", "Transition", 2.54, 1064, 2856, 19.3, 10, 5},
    {79, "Hg", "Mercury", 200.6, 12, 6, "d", "[Xe] 4f14 5d10 6s2", "+2,+1", "Liquid", "Transition", 2.00, -38.83, 356.7, 13.5, 11, 5},
    {80, "Tl", "Thallium", 204.4, 13, 6, "p", "[Xe] 4f14 5d10 6s2 6p1", "+3,+1", "Solid", "Post-transition", 1.62, 304, 1473, 11.9, 12, 5},
    {81, "Pb", "Lead", 207.2, 14, 6, "p", "[Xe] 4f14 5d10 6s2 6p2", "+4,+2", "Solid", "Post-transition", 2.33, 327.5, 1749, 11.3, 13, 5},
    {82, "Bi", "Bismuth", 209.0, 15, 6, "p", "[Xe] 4f14 5d10 6s2 6p3", "+5,+3,-3", "Solid", "Post-transition", 2.02, 271.4, 1564, 9.78, 14, 5},
    {83, "Po", "Polonium", 209.0, 16, 6, "p", "[Xe] 4f14 5d10 6s2 6p4", "+4,+2", "Solid", "Metalloid", 2.0, 254, 962, 9.2, 15, 5},
    {84, "At", "Astatine", 210.0, 17, 6, "p", "[Xe] 4f14 5d10 6s2 6p5", "+7,+5,+1,-1", "Solid", "Halogen", 2.2, 302, 337, 7.0, 16, 5},
    {85, "Rn", "Radon", 222.0, 18, 6, "p", "[Xe] 4f14 5d10 6s2 6p6", "0", "Gas", "Noble gas", 2.6, -71, -61.7, 0.0097, 17, 5},
    {86, "Fr", "Francium", 223.0, 1, 7, "s", "[Rn] 7s1", "+1", "Solid", "Alkali metal", 0.7, 27, 677, 2.48, 0, 6},
    {87, "Ra", "Radium", 226.0, 2, 7, "s", "[Rn] 7s2", "+2", "Solid", "Alkaline earth", 0.9, 700, 1737, 5.5, 1, 6},
    {88, "Ac", "Actinium", 227.0, 3, 7, "f", "[Rn] 6d1 7s2", "+3", "Solid", "Actinide", 1.1, 1050, 3200, 10.1, 2, 6},
    {89, "Th", "Thorium", 232.0, 3, 7, "f", "[Rn] 6d2 7s2", "+4", "Solid", "Actinide", 1.3, 1750, 4788, 11.7, 3, 6},
    {90, "Pa", "Protactinium", 231.0, 3, 7, "f", "[Rn] 5f2 6d1 7s2", "+5", "Solid", "Actinide", 1.5, 1568, 4027, 15.4, 4, 6},
    {91, "U", "Uranium", 238.0, 3, 7, "f", "[Rn] 5f3 6d1 7s2", "+6,+4", "Solid", "Actinide", 1.38, 1135, 4131, 19.1, 5, 6},
    {92, "Np", "Neptunium", 237.0, 3, 7, "f", "[Rn] 5f4 6d1 7s2", "+5", "Solid", "Actinide", 1.36, 644, 3902, 20.5, 6, 6},
    {93, "Pu", "Plutonium", 244.0, 3, 7, "f", "[Rn] 5f6 7s2", "+4", "Solid", "Actinide", 1.28, 640, 3228, 19.8, 7, 6},
    {94, "Am", "Americium", 243.0, 3, 7, "f", "[Rn] 5f7 7s2", "+3", "Solid", "Actinide", 1.3, 1176, 2011, 12.0, 8, 6},
    {95, "Cm", "Curium", 247.0, 3, 7, "f", "[Rn] 5f7 6d1 7s2", "+3", "Solid", "Actinide", 1.3, 1340, 3110, 13.5, 9, 6},
    {96, "Bk", "Berkelium", 247.0, 3, 7, "f", "[Rn] 5f9 7s2", "+3,+4", "Solid", "Actinide", 1.3, 986, 2627, 14.8, 10, 6},
    {97, "Cf", "Californium", 251.0, 3, 7, "f", "[Rn] 5f10 7s2", "+3", "Solid", "Actinide", 1.3, 900, 1743, 15.1, 11, 6},
    {98, "Es", "Einsteinium", 252.0, 3, 7, "f", "[Rn] 5f11 7s2", "+3", "Solid", "Actinide", 1.3, 860, 1270, 13.5, 12, 6},
    {99, "Fm", "Fermium", 257.0, 3, 7, "f", "[Rn] 5f12 7s2", "+3", "Solid", "Actinide", 1.3, 1527, 1270, 13.5, 13, 6},
    {100, "Md", "Mendelevium", 258.0, 3, 7, "f", "[Rn] 5f13 7s2", "+3,+2", "Solid", "Actinide", 1.3, 827, 1270, 13.5, 14, 6},
    {101, "No", "Nobelium", 259.0, 3, 7, "f", "[Rn] 5f14 7s2", "+3,+2", "Solid", "Actinide", 1.3, 827, 1270, 13.5, 15, 6},
    {102, "Lr", "Lawrencium", 262.0, 3, 7, "d", "[Rn] 5f14 7s2 7p1", "+3", "Solid", "Actinide", 1.3, 1627, 1270, 13.5, 16, 6},
    {103, "Rf", "Rutherfordium", 267.0, 4, 7, "d", "[Rn] 5f14 6d2 7s2", "+4", "Solid", "Transition", 1.3, 2100, 1270, 13.5, 3, 6},
    {104, "Db", "Dubnium", 268.0, 5, 7, "d", "[Rn] 5f14 6d3 7s2", "+5", "Solid", "Transition", 1.3, 1270, 1270, 13.5, 4, 6},
    {105, "Sg", "Seaborgium", 269.0, 6, 7, "d", "[Rn] 5f14 6d4 7s2", "+6", "Solid", "Transition", 1.3, 1270, 1270, 13.5, 5, 6},
    {106, "Bh", "Bohrium", 270.0, 7, 7, "d", "[Rn] 5f14 6d5 7s2", "+7", "Solid", "Transition", 1.3, 1270, 1270, 13.5, 6, 6},
    {107, "Hs", "Hassium", 277.0, 8, 7, "d", "[Rn] 5f14 6d6 7s2", "+8", "Solid", "Transition", 1.3, 1270, 1270, 13.5, 7, 6},
    {108, "Mt", "Meitnerium", 278.0, 9, 7, "d", "[Rn] 5f14 6d7 7s2", "+3", "Solid", "Transition", 1.3, 1270, 1270, 13.5, 8, 6},
    {109, "Ds", "Darmstadtium", 281.0, 10, 7, "d", "[Rn] 5f14 6d8 7s2", "+2", "Solid", "Transition", 1.3, 1270, 1270, 13.5, 9, 6},
    {110, "Rg", "Roentgenium", 282.0, 11, 7, "d", "[Rn] 5f14 6d10 7s1", "+3", "Solid", "Transition", 1.3, 1270, 1270, 13.5, 10, 6},
    {111, "Cn", "Copernicium", 285.0, 12, 7, "d", "[Rn] 5f14 6d10 7s2", "+2", "Solid", "Transition", 1.3, 1270, 1270, 13.5, 11, 6},
    {112, "Nh", "Nihonium", 286.0, 13, 7, "p", "[Rn] 5f14 6d10 7s2 7p1", "+1", "Solid", "Post-transition", 1.3, 1270, 1270, 13.5, 12, 6},
    {113, "Fl", "Flerovium", 289.0, 14, 7, "p", "[Rn] 5f14 6d10 7s2 7p2", "+2", "Solid", "Post-transition", 1.3, 1270, 1270, 13.5, 13, 6},
    {114, "Mc", "Moscovium", 290.0, 15, 7, "p", "[Rn] 5f14 6d10 7s2 7p3", "+3", "Solid", "Post-transition", 1.3, 1270, 1270, 13.5, 14, 6},
    {115, "Lv", "Livermorium", 293.0, 16, 7, "p", "[Rn] 5f14 6d10 7s2 7p4", "+2", "Solid", "Post-transition", 1.3, 1270, 1270, 13.5, 15, 6},
    {116, "Ts", "Tennessine", 294.0, 17, 7, "p", "[Rn] 5f14 6d10 7s2 7p5", "+1", "Solid", "Halogen", 1.3, 1270, 1270, 13.5, 16, 6},
    {117, "Og", "Oganesson", 294.0, 18, 7, "p", "[Rn] 5f14 6d10 7s2 7p6", "0", "Gas", "Noble gas", 1.3, 1270, 1270, 13.5, 17, 6},
};
 
typedef struct {
    chem_tool_t tool;
    int ptable_sel;
    int ptable_scroll_x, ptable_scroll_y;
    int info_scroll;
    char input[CHEM_INPUT_MAX];
    int input_len, cursor;
    bool editing;
    bool show_tool_menu;
    int tool_menu_sel;
    bool show_help;
} chem_state_t;

static const char *chem_tool_names[CHEM_TOOL_COUNT] = {
    "Periodic Table", "Molar Mass", "pH", "Gas Laws", "Stoichiometry",
    "Thermochemistry", "Organic Chemistry", "Redox Reactions",
    "Bonding / Lewis", "Electron Config", "Equation Balance", "Titration"
};

static void chem_draw_periodic(ili9488_t *lcd, const ui_rect_t *r, chem_state_t *st) {
    ui_draw_fill_rect(lcd, r, UI_C_WIN_BG);
    ui_draw_outline(lcd, r, UI_C_BORDER);
    ui_draw_text(lcd, r->x + 4, r->y + 2, "Periodic Table", UI_C_TEXT_DIM, UI_C_WIN_BG);

    int cell_w = (r->w - 8) / CHEM_PTABLE_COLS;
    int cell_h = (r->h - 24) / CHEM_PTABLE_ROWS;
    if (cell_w < 16) cell_w = 16;
    if (cell_h < 14) cell_h = 14;

    for (int i = 0; i < CHEM_MAX_ELEMENTS; i++) {
        const chem_element_t *e = &g_elements[i];
        int cx = r->x + 4 + e->ptable_x * cell_w;
        int cy = r->y + 20 + e->ptable_y * cell_h;
        if (cx + cell_w > r->x + r->w - 4 || cy + cell_h > r->y + r->h - 4) continue;

        bool sel = (st->ptable_sel == i);
        ui_rect_t cell = {cx, cy, (uint16_t)cell_w, (uint16_t)cell_h};
        uint16_t fill_col = cat_color(e->category);
        uint16_t txt_col = (strcmp(e->category, "Noble gas") == 0) ? UI_C_WIN_BG : UI_C_TEXT_LIGHT;

        if (sel) {
            ui_draw_fill_rect(lcd, &cell, UI_C_ACCENT);
            ui_draw_text(lcd, cx + 2, cy + 1, e->symbol, UI_C_WIN_BG, UI_C_ACCENT);
        } else {
            ui_draw_fill_rect(lcd, &cell, fill_col);
            ui_draw_outline(lcd, &cell, UI_C_BORDER);
            ui_draw_text(lcd, cx + 2, cy + 1, e->symbol, txt_col, fill_col);
        }
    }
}

static void chem_draw_element_info(ili9488_t *lcd, const ui_rect_t *r, chem_state_t *st) {
    ui_draw_fill_rect(lcd, r, UI_C_WIN_BG);
    ui_draw_outline(lcd, r, UI_C_BORDER);
    draw_small_text(lcd, r->x + 4, r->y + 2, "Element Information", UI_C_TEXT_DIM, UI_C_WIN_BG);

    const chem_element_t *e = &g_elements[st->ptable_sel];
    int y = r->y + 12;
    int max_chars = (r->w - 12) / SF_W;
    if (max_chars < 10) max_chars = 10;
    int bottom_limit = r->y + r->h - 4 - (SF_H + 2);

    char buf[80];
    snprintf(buf, sizeof(buf), "%s (%s)", e->name, e->symbol);
    draw_wrapped_small(lcd, r->x + 4, &y, buf, UI_C_TEXT, max_chars, bottom_limit);
    snprintf(buf, sizeof(buf), "Atomic #: %d", e->atomic_num);
    draw_wrapped_small(lcd, r->x + 4, &y, buf, UI_C_TEXT, max_chars, bottom_limit);
    snprintf(buf, sizeof(buf), "Mass: %.3f u", e->mass);
    draw_wrapped_small(lcd, r->x + 4, &y, buf, UI_C_TEXT, max_chars, bottom_limit);
    snprintf(buf, sizeof(buf), "Group: %d", e->group);
    draw_wrapped_small(lcd, r->x + 4, &y, buf, UI_C_TEXT, max_chars, bottom_limit);
    snprintf(buf, sizeof(buf), "Period: %d", e->period);
    draw_wrapped_small(lcd, r->x + 4, &y, buf, UI_C_TEXT, max_chars, bottom_limit);
    snprintf(buf, sizeof(buf), "Block: %s", e->block);
    draw_wrapped_small(lcd, r->x + 4, &y, buf, UI_C_TEXT, max_chars, bottom_limit);
    snprintf(buf, sizeof(buf), "Config: %s", e->electron_config);
    draw_wrapped_small(lcd, r->x + 4, &y, buf, UI_C_TEXT, max_chars, bottom_limit);
    snprintf(buf, sizeof(buf), "Ox. States: %s", e->oxidation_states);
    draw_wrapped_small(lcd, r->x + 4, &y, buf, UI_C_TEXT, max_chars, bottom_limit);
    snprintf(buf, sizeof(buf), "State: %s", e->state);
    draw_wrapped_small(lcd, r->x + 4, &y, buf, UI_C_TEXT, max_chars, bottom_limit);
    snprintf(buf, sizeof(buf), "Category: %s", e->category);
    draw_wrapped_small(lcd, r->x + 4, &y, buf, UI_C_TEXT, max_chars, bottom_limit);
    if (e->electronegativity > 0) {
        snprintf(buf, sizeof(buf), "EN: %.2f", e->electronegativity);
        draw_wrapped_small(lcd, r->x + 4, &y, buf, UI_C_TEXT, max_chars, bottom_limit);
    }
    if (e->melting_point > -273) {
        snprintf(buf, sizeof(buf), "Melt: %.1f C", e->melting_point);
        draw_wrapped_small(lcd, r->x + 4, &y, buf, UI_C_TEXT, max_chars, bottom_limit);
    }
    if (e->boiling_point > -273) {
        snprintf(buf, sizeof(buf), "Boil: %.1f C", e->boiling_point);
        draw_wrapped_small(lcd, r->x + 4, &y, buf, UI_C_TEXT, max_chars, bottom_limit);
    }
    if (e->density > 0) {
        snprintf(buf, sizeof(buf), "Density: %.3f g/cm3", e->density);
        draw_wrapped_small(lcd, r->x + 4, &y, buf, UI_C_TEXT, max_chars, bottom_limit);
    }
    draw_wrapped_small(lcd, r->x + 4, &y, "Atomic radius: N/A", UI_C_TEXT, max_chars, bottom_limit);
    draw_wrapped_small(lcd, r->x + 4, &y, "Covalent radius: N/A", UI_C_TEXT, max_chars, bottom_limit);
    draw_wrapped_small(lcd, r->x + 4, &y, "vdW radius: N/A", UI_C_TEXT, max_chars, bottom_limit);
    draw_wrapped_small(lcd, r->x + 4, &y, "Ionization energy: N/A", UI_C_TEXT, max_chars, bottom_limit);
    draw_wrapped_small(lcd, r->x + 4, &y, "Electron affinity: N/A", UI_C_TEXT, max_chars, bottom_limit);
    draw_wrapped_small(lcd, r->x + 4, &y, "Thermal conductivity: N/A", UI_C_TEXT, max_chars, bottom_limit);
    draw_wrapped_small(lcd, r->x + 4, &y, "Electrical conductivity: N/A", UI_C_TEXT, max_chars, bottom_limit);
    draw_wrapped_small(lcd, r->x + 4, &y, "Specific heat: N/A", UI_C_TEXT, max_chars, bottom_limit);
}

static void chem_draw_tool_menu(ili9488_t *lcd, const ui_rect_t *r) {
    ui_draw_fill_rect(lcd, r, UI_C_PANEL);
    ui_draw_outline(lcd, r, UI_C_ACCENT);
    ui_draw_text(lcd, r->x + 8, r->y + 6, "Chemistry Tools", UI_C_TEXT, UI_C_PANEL);
    int y = r->y + 24;
    for (int i = 0; i < CHEM_TOOL_COUNT; i++) {
        ui_draw_text(lcd, r->x + 12, y, chem_tool_names[i], UI_C_TEXT, UI_C_PANEL);
        y += CHAR_H + 2;
    }
}

static void chem_draw_help(ili9488_t *lcd, const ui_rect_t *r) {
    ui_draw_fill_rect(lcd, r, UI_C_PANEL);
    ui_draw_outline(lcd, r, UI_C_ACCENT);
    const char *lines[] = {
        "Chemistry Shortcuts:",
        "  W/A/S/D   Move in periodic table",
        "  E         Select element",
        "  F4        Open tool menu",
        "  F7        Toggle this help",
        "  F10       Fullscreen",
        "  Esc       Back / Cancel",
    };
    int y = r->y + 6;
    for (size_t i = 0; i < sizeof(lines)/sizeof(lines[0]); i++) {
        ui_draw_text(lcd, r->x + 8, y, lines[i], UI_C_TEXT, UI_C_PANEL);
        y += CHAR_H + 2;
    }
}

static void chem_init(arpile_app_ctx_t *ctx) {
    chem_state_t *st = malloc(sizeof(chem_state_t));
    if (!st) return;
    memset(st, 0, sizeof(chem_state_t));
    st->tool = CHEM_TOOL_PERIODIC;
    st->ptable_sel = 5;
    ctx->user = st;
    if (ctx->win) ctx->win->state.capture_nav = true;
}

static void chem_destroy(arpile_app_ctx_t *ctx) {
    chem_state_t *st = ctx->user;
    free(st);
    ctx->user = NULL;
}

static void chem_render(arpile_app_ctx_t *ctx, ui_win_t *win) {
    chem_state_t *st = ctx->user;
    if (!st) { chem_init(ctx); st = ctx->user; if (!st) return; }

    ili9488_t *lcd = arpile_ui_get_lcd();
    ui_rect_t c = win_client_rect(win);
    ui_draw_fill_rect(lcd, &c, UI_C_WIN_BG);

    int left_w = 300;
    int right_w = c.w - left_w;

    ui_rect_t left = { c.x, c.y, (uint16_t)left_w, c.h };
    ui_rect_t right = { (uint16_t)(c.x + left_w), c.y, (uint16_t)right_w, c.h };

    if (st->tool == CHEM_TOOL_PERIODIC) {
        chem_draw_periodic(lcd, &left, st);
        chem_draw_element_info(lcd, &right, st);
    } else {
        ui_draw_fill_rect(lcd, &left, UI_C_WIN_BG);
        ui_draw_text(lcd, left.x + 4, left.y + 4, chem_tool_names[st->tool], UI_C_TEXT, UI_C_WIN_BG);
        ui_draw_text(lcd, left.x + 4, left.y + 20, "Tool not yet implemented", UI_C_TEXT_DIM, UI_C_WIN_BG);
        ui_draw_fill_rect(lcd, &right, UI_C_WIN_BG);
    }

    if (st->show_tool_menu) {
        ui_rect_t tm = { c.x + 40, c.y + 20, 200, 240 };
        chem_draw_tool_menu(lcd, &tm);
    }
    if (st->show_help) {
        ui_rect_t hp = { c.x + 40, c.y + 20, 240, 160 };
        chem_draw_help(lcd, &hp);
    }
}

static void chem_event(arpile_app_ctx_t *ctx, const arpile_input_event_t *ev) {
    chem_state_t *st = ctx->user;
    if (!st) return;
    if (ev->type != ARPILE_IN_EVENT_KEY_DOWN) return;
    uint16_t key = ev->key.keycode;
    uint8_t mod = ev->key.modifier;
    (void)mod;

    if (key == ARPILE_KEY_F4) {
        st->show_tool_menu = !st->show_tool_menu;
        arpile_ui_win_redraw(ctx->win);
        return;
    }
    if (key == ARPILE_KEY_F7) {
        st->show_help = !st->show_help;
        arpile_ui_win_redraw(ctx->win);
        return;
    }
    if (key == ARPILE_KEY_F10) return;
    if (key == ARPILE_KEY_ESCAPE) {
        if (st->show_help) { st->show_help = false; arpile_ui_win_redraw(ctx->win); return; }
        if (st->show_tool_menu) { st->show_tool_menu = false; arpile_ui_win_redraw(ctx->win); return; }
    }

    if (st->tool == CHEM_TOOL_PERIODIC) {
        if (key == ARPILE_KEY_W) {
            int best = -1, best_dist = 999;
            for (int i = 0; i < CHEM_MAX_ELEMENTS; i++) {
                if (g_elements[i].ptable_y < g_elements[st->ptable_sel].ptable_y) {
                    int dist = g_elements[st->ptable_sel].ptable_y - g_elements[i].ptable_y;
                    int hdist = abs(g_elements[i].ptable_x - g_elements[st->ptable_sel].ptable_x);
                    if (dist < best_dist || (dist == best_dist && hdist < abs(g_elements[best].ptable_x - g_elements[st->ptable_sel].ptable_x))) {
                        best_dist = dist; best = i;
                    }
                }
            }
            if (best >= 0) { st->ptable_sel = best; arpile_ui_win_redraw(ctx->win); }
            return;
        }
        if (key == ARPILE_KEY_S) {
            int best = -1, best_dist = 999;
            for (int i = 0; i < CHEM_MAX_ELEMENTS; i++) {
                if (g_elements[i].ptable_y > g_elements[st->ptable_sel].ptable_y) {
                    int dist = g_elements[i].ptable_y - g_elements[st->ptable_sel].ptable_y;
                    int hdist = abs(g_elements[i].ptable_x - g_elements[st->ptable_sel].ptable_x);
                    if (dist < best_dist || (dist == best_dist && hdist < abs(g_elements[best].ptable_x - g_elements[st->ptable_sel].ptable_x))) {
                        best_dist = dist; best = i;
                    }
                }
            }
            if (best >= 0) { st->ptable_sel = best; arpile_ui_win_redraw(ctx->win); }
            return;
        }
        if (key == ARPILE_KEY_A) {
            int best = -1, best_dist = 999;
            for (int i = 0; i < CHEM_MAX_ELEMENTS; i++) {
                if (g_elements[i].ptable_y == g_elements[st->ptable_sel].ptable_y && g_elements[i].ptable_x < g_elements[st->ptable_sel].ptable_x) {
                    int dist = g_elements[st->ptable_sel].ptable_x - g_elements[i].ptable_x;
                    if (dist < best_dist) { best_dist = dist; best = i; }
                }
            }
            if (best >= 0) { st->ptable_sel = best; arpile_ui_win_redraw(ctx->win); }
            return;
        }
        if (key == ARPILE_KEY_D) {
            int best = -1, best_dist = 999;
            for (int i = 0; i < CHEM_MAX_ELEMENTS; i++) {
                if (g_elements[i].ptable_y == g_elements[st->ptable_sel].ptable_y && g_elements[i].ptable_x > g_elements[st->ptable_sel].ptable_x) {
                    int dist = g_elements[i].ptable_x - g_elements[st->ptable_sel].ptable_x;
                    if (dist < best_dist) { best_dist = dist; best = i; }
                }
            }
            if (best >= 0) { st->ptable_sel = best; arpile_ui_win_redraw(ctx->win); }
            return;
        }
        if (key == ARPILE_KEY_E) {
            arpile_ui_win_redraw(ctx->win);
            return;
        }
    }
}

static const arpile_app_ops_t chem_ops = {
    .init = chem_init,
    .event = chem_event,
    .render = chem_render,
    .destroy = chem_destroy,
};

const arpile_app_t arpile_app_chem = {
    .id = "chem",
    .name = "Chemistry",
    .icon = "chem",
    .ops = &chem_ops,
};