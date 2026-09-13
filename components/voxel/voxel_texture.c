#include "voxel_texture.h"

static uint32_t s_rng = 0x9E3779B9u;

static uint32_t rng_next(void)
{
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void fill_base(uint16_t *tile, uint8_t r, uint8_t g, uint8_t b,
                      int8_t noise, int8_t speckle)
{
    for (int i = 0; i < VOX_TILE * VOX_TILE; i++) {
        int d = (int)(rng_next() % 17) - 8;
        if (d < 0) d = -d;
        int8_t n = (int8_t)((d > noise) ? 0 : d);
        int nv = (int)(rng_next() % 3) - 1;
        if (nv > 0 && (int)(rng_next() % speckle) == 0) n = speckle;
        int rr = (int)r + n * nv;
        int gg = (int)g + n * nv;
        int bb = (int)b + n * nv;
        if (rr < 0) rr = 0; else if (rr > 255) rr = 255;
        if (gg < 0) gg = 0; else if (gg > 255) gg = 255;
        if (bb < 0) bb = 0; else if (bb > 255) bb = 255;
        tile[i] = rgb565((uint8_t)rr, (uint8_t)gg, (uint8_t)bb);
    }
}

static void fill_grass_side(uint16_t *tile)
{
    fill_base(tile, 122, 90, 60, 4, 3);
    for (int ry = 0; ry < 4; ry++) {
        for (int x = 0; x < VOX_TILE; x++) {
            int i = ry * VOX_TILE + x;
            int n = (int)(rng_next() % 5) - 2;
            int rr = 106 + n;
            int gg = 158 + n;
            int bb = 66 + n;
            if (rr < 0) rr = 0; else if (rr > 255) rr = 255;
            if (gg < 0) gg = 0; else if (gg > 255) gg = 255;
            if (bb < 0) bb = 0; else if (bb > 255) bb = 255;
            tile[i] = rgb565((uint8_t)rr, (uint8_t)gg, (uint8_t)bb);
        }
    }
}

static void fill_stone(uint16_t *tile)
{
    fill_base(tile, 128, 128, 128, 5, 4);
    for (int k = 0; k < 4; k++) {
        int cx = 2 + (int)(rng_next() % (VOX_TILE - 4));
        int cy = 2 + (int)(rng_next() % (VOX_TILE - 4));
        int len = 3 + (int)(rng_next() % 5);
        int h = (rng_next() & 1) ? 1 : -1;
        int v = (rng_next() & 1) ? 1 : -1;
        for (int s = 0; s < len; s++) {
            int x = cx + s * h;
            int y = cy + s * v;
            if (x < 0 || x >= VOX_TILE || y < 0 || y >= VOX_TILE) break;
            tile[y * VOX_TILE + x] = rgb565(96, 96, 96);
        }
    }
}

static void fill_sand(uint16_t *tile)
{
    fill_base(tile, 219, 207, 166, 3, 3);
    for (int k = 0; k < 6; k++) {
        int cx = (int)(rng_next() % VOX_TILE);
        int cy = (int)(rng_next() % VOX_TILE);
        tile[cy * VOX_TILE + cx] = rgb565(205, 192, 150);
    }
}

static void fill_wood_top(uint16_t *tile)
{
    int mid = VOX_TILE / 2 - 1;
    for (int y = 0; y < VOX_TILE; y++) {
        for (int x = 0; x < VOX_TILE; x++) {
            int dx = x - mid;
            int dy = y - mid;
            int d = (dx * dx >= dy * dy) ? dx : dy;
            int ring = (int)(rng_next() % 2);
            int wood = (d & 1) ? 110 + ring : 96 + ring;
            int dark = (rng_next() % 24 == 0) ? 30 : 0;
            tile[y * VOX_TILE + x] = rgb565((uint8_t)(wood - dark),
                                            (uint8_t)(85 - dark),
                                            (uint8_t)(48 - dark));
        }
    }
}

static void fill_wood_side(uint16_t *tile)
{
    for (int y = 0; y < VOX_TILE; y++) {
        for (int x = 0; x < VOX_TILE; x++) {
            int stripe = (x + (int)(rng_next() % 3) - 1) / 2;
            int bright = (stripe & 1) ? 122 : 102;
            int grain = (int)(rng_next() % 3) - 1;
            tile[y * VOX_TILE + x] = rgb565((uint8_t)(bright + grain),
                                            (uint8_t)(88 + grain),
                                            (uint8_t)(52 + grain));
        }
    }
}

static void fill_leaves(uint16_t *tile)
{
    for (int y = 0; y < VOX_TILE; y++) {
        for (int x = 0; x < VOX_TILE; x++) {
            int n = (int)(rng_next() % 5) - 2;
            int dark = (rng_next() % 8 == 0) ? 26 : 0;
            tile[y * VOX_TILE + x] = rgb565((uint8_t)(58 + n - dark),
                                            (uint8_t)(116 + n - dark),
                                            (uint8_t)(38 + n - dark));
        }
    }
}

void vox_tex_init(vox_tex_atlas *atlas)
{
    uint16_t base[VOX_TILE * VOX_TILE];
    fill_base(base, 96, 152, 78, 4, 3);
    for (int i = 0; i < VOX_TILE * VOX_TILE; i++) {
        atlas->atlas[VOX_TEX_GRASS * VOX_TILE * VOX_TILE + i] = base[i];
    }
    fill_base(base, 122, 90, 60, 4, 3);
    for (int i = 0; i < VOX_TILE * VOX_TILE; i++) {
        atlas->atlas[VOX_TEX_DIRT * VOX_TILE * VOX_TILE + i] = base[i];
    }
    fill_stone(base);
    for (int i = 0; i < VOX_TILE * VOX_TILE; i++) {
        atlas->atlas[VOX_TEX_STONE * VOX_TILE * VOX_TILE + i] = base[i];
    }
    fill_grass_side(base);
    for (int i = 0; i < VOX_TILE * VOX_TILE; i++) {
        atlas->atlas[VOX_TEX_GRASS_SIDE * VOX_TILE * VOX_TILE + i] = base[i];
    }
    fill_sand(base);
    for (int i = 0; i < VOX_TILE * VOX_TILE; i++) {
        atlas->atlas[VOX_TEX_SAND * VOX_TILE * VOX_TILE + i] = base[i];
    }
    fill_wood_top(base);
    for (int i = 0; i < VOX_TILE * VOX_TILE; i++) {
        atlas->atlas[VOX_TEX_WOOD_TOP * VOX_TILE * VOX_TILE + i] = base[i];
    }
    fill_wood_side(base);
    for (int i = 0; i < VOX_TILE * VOX_TILE; i++) {
        atlas->atlas[VOX_TEX_WOOD_SIDE * VOX_TILE * VOX_TILE + i] = base[i];
    }
    fill_leaves(base);
    for (int i = 0; i < VOX_TILE * VOX_TILE; i++) {
        atlas->atlas[VOX_TEX_LEAVES * VOX_TILE * VOX_TILE + i] = base[i];
    }
}

uint16_t vox_tex_sample(const vox_tex_atlas *atlas, vox_tex_id id, int tx, int ty)
{
    int x = tx & (VOX_TILE - 1);
    int y = ty & (VOX_TILE - 1);
    return atlas->atlas[id * VOX_TILE * VOX_TILE + y * VOX_TILE + x];
}

void vox_tex_shade_tile(const vox_tex_atlas *atlas, vox_tex_id id, float shade,
                        uint16_t out[VOX_TILE * VOX_TILE])
{
    int sc = (int)(shade * 255.0f);
    if (sc < 0) sc = 0;
    if (sc > 255) sc = 255;
    for (int i = 0; i < VOX_TILE * VOX_TILE; i++) {
        uint16_t p = atlas->atlas[id * VOX_TILE * VOX_TILE + i];
        int r = (p >> 11) & 0x1F;
        int g = (p >> 5) & 0x3F;
        int b = p & 0x1F;
        r = (r * sc) >> 8;
        g = (g * sc) >> 8;
        b = (b * sc) >> 8;
        out[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}