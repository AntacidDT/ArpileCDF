#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "voxel_renderer.h"

#define NEAR_PLANE 0.1f
#define MAX_DEPTH 65535.0f

/* One float division per pixel is replaced by one reciprocal + multiplies:
 * u = uq/q  ->  ui = uq * (1/q).  The reciprocal is the only div per pixel. */
static void set_pixel_uv(vox_renderer *r, int x, int y, float q,
                         float uq, float vq, const uint16_t *tile)
{
    uint16_t *row = r->fb + (size_t)y * r->width;
    uint16_t *zrow = r->zbuf + (size_t)y * r->width;
    int z16 = (int)(q * r->zscale);
    if (z16 > (int)MAX_DEPTH) z16 = (int)MAX_DEPTH;
    if (z16 < 0) z16 = 0;
    if (z16 > zrow[x]) {
        float iz = 1.0f / q;
        int tx = (int)(uq * iz) & (VOX_TILE - 1);
        int ty = (int)(vq * iz) & (VOX_TILE - 1);
        row[x] = tile[ty * VOX_TILE + tx];
        zrow[x] = (uint16_t)z16;
        r->frags_drawn++;
    }
    r->frags_tested++;
}

static void edge_sample(const vox_vtx *a, const vox_vtx *b, float y,
                        vox_vtx *out)
{
    float dy = b->y - a->y;
    float t = (dy > 1e-6f) ? (y - a->y) / dy : 0.5f;
    if (t < 0.0f) t = 0.0f;
    else if (t > 1.0f) t = 1.0f;
    out->x = a->x + (b->x - a->x) * t;
    out->q = a->q + (b->q - a->q) * t;
    out->uq = a->uq + (b->uq - a->uq) * t;
    out->vq = a->vq + (b->vq - a->vq) * t;
}

static void draw_span(vox_renderer *r, int y, vox_vtx l, vox_vtx rr,
                      const uint16_t *tile)
{
    float span = rr.x - l.x;
    if (span < 1e-6f) return;
    float dq = (rr.q - l.q) / span;
    float du = (rr.uq - l.uq) / span;
    float dv = (rr.vq - l.vq) / span;
    float q = l.q;
    float uq = l.uq;
    float vq = l.vq;

    int x0 = (int)ceilf(l.x);
    int x1 = (int)ceilf(rr.x) - 1;
    if (x0 < 0) x0 = 0;
    if (x1 >= r->width) x1 = r->width - 1;
    if (x0 > x1) return;

    q += (x0 - l.x) * dq;
    uq += (x0 - l.x) * du;
    vq += (x0 - l.x) * dv;

    for (int x = x0; x <= x1; x++) {
        set_pixel_uv(r, x, y, q, uq, vq, tile);
        q += dq;
        uq += du;
        vq += dv;
    }
}

void vox_renderer_triangle(vox_renderer *r, const vox_vtx *a, const vox_vtx *b,
                           const vox_vtx *c, const uint16_t *tile)
{
    int64_t t0 = esp_timer_get_time();
    const vox_vtx *v[3] = { a, b, c };
    if (v[0]->y > v[1]->y) {
        const vox_vtx *t = v[0]; v[0] = v[1]; v[1] = t;
    }
    if (v[1]->y > v[2]->y) {
        const vox_vtx *t = v[1]; v[1] = v[2]; v[2] = t;
    }
    if (v[0]->y > v[1]->y) {
        const vox_vtx *t = v[0]; v[0] = v[1]; v[1] = t;
    }

    int y0 = (int)ceilf(v[0]->y);
    int y2 = (int)ceilf(v[2]->y) - 1;
    if (y0 < 0) y0 = 0;
    if (y2 >= r->height) y2 = r->height - 1;
    if (y0 > y2) return;

    r->tris_drawn++;
    vox_vtx edge_long, edge_short;
    vox_vtx l, rr;
    for (int y = y0; y <= y2; y++) {
        float fy = (float)y;
        edge_sample(v[0], v[2], fy, &edge_long);
        if (fy < v[1]->y) {
            edge_sample(v[0], v[1], fy, &edge_short);
        } else {
            edge_sample(v[1], v[2], fy, &edge_short);
        }
        if (edge_short.x < edge_long.x) {
            l = edge_short;
            rr = edge_long;
        } else {
            l = edge_long;
            rr = edge_short;
        }
        draw_span(r, y, l, rr, tile);
    }
    r->raster_us += (uint32_t)(esp_timer_get_time() - t0);
}

int vox_renderer_resize(vox_renderer *r, int width, int height)
{
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (r->fb && r->width == width && r->height == height) {
        return 0;
    }
    vox_renderer_destroy(r);

    r->width = width;
    r->height = height;
    r->zbuf = NULL;
    r->fb = NULL;

    size_t fbsz = (size_t)width * (size_t)height * sizeof(uint16_t);
    r->fb = heap_caps_malloc(fbsz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r->fb) {
        r->fb = heap_caps_malloc(fbsz, MALLOC_CAP_8BIT);
    }
    r->zbuf = heap_caps_malloc(fbsz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r->zbuf) {
        r->zbuf = heap_caps_malloc(fbsz, MALLOC_CAP_8BIT);
    }
    if (!r->fb || !r->zbuf) {
        vox_renderer_destroy(r);
        return -1;
    }

    float near_inv = 1.0f / NEAR_PLANE;
    r->zscale = MAX_DEPTH / near_inv;
    r->sky_top = 0x5417;
    r->sky_bottom = 0xA64D;
    return 0;
}

void vox_renderer_begin(vox_renderer *r)
{
    r->frags_drawn = 0;
    r->frags_tested = 0;
    r->tris_drawn = 0;
    r->raster_us = 0;
    int64_t t0 = esp_timer_get_time();

    for (int y = 0; y < r->height; y++) {
        float t = (r->height > 1) ? (float)y / (float)(r->height - 1) : 0.0f;
        int rt = (r->sky_top >> 11) & 0x1F;
        int gt = (r->sky_top >> 5) & 0x3F;
        int bt = r->sky_top & 0x1F;
        int rb = (r->sky_bottom >> 11) & 0x1F;
        int gb = (r->sky_bottom >> 5) & 0x3F;
        int bb = r->sky_bottom & 0x1F;
        int rr = (int)(rt + (float)(rb - rt) * t);
        int gg = (int)(gt + (float)(gb - gt) * t);
        int bb2 = (int)(bt + (float)(bb - bt) * t);
        uint16_t color = (uint16_t)((rr << 11) | (gg << 5) | bb2);
        uint16_t *row = r->fb + (size_t)y * r->width;
        for (int x = 0; x < r->width; x++) {
            row[x] = color;
        }
        memset(r->zbuf + (size_t)y * r->width, 0, r->width * sizeof(uint16_t));
    }
    r->begin_us = (uint32_t)(esp_timer_get_time() - t0);
}

void vox_renderer_destroy(vox_renderer *r)
{
    if (r->fb) {
        heap_caps_free(r->fb);
        r->fb = NULL;
    }
    if (r->zbuf) {
        heap_caps_free(r->zbuf);
        r->zbuf = NULL;
    }
    r->width = 0;
    r->height = 0;
}