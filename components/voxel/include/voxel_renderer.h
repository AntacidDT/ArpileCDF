#pragma once

#include <stdint.h>
#include "voxel_texture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x, y;
    float q;
    float uq, vq;
} vox_vtx;

typedef struct {
    uint16_t *fb;
    uint16_t *zbuf;
    int width;
    int height;
    float zscale;
    uint16_t sky_top;
    uint16_t sky_bottom;
    /* Per-frame performance counters (reset by vox_renderer_begin). */
    uint32_t frags_drawn;   /* depth-test passing pixels written */
    uint32_t frags_tested;  /* depth-test attempts */
    uint32_t tris_drawn;    /* rasterized triangle count */
    uint32_t raster_us;     /* microseconds inside vox_renderer_triangle */
    uint32_t begin_us;      /* microseconds inside vox_renderer_begin */
} vox_renderer;

int vox_renderer_resize(vox_renderer *r, int width, int height);

void vox_renderer_destroy(vox_renderer *r);

void vox_renderer_begin(vox_renderer *r);

void vox_renderer_triangle(vox_renderer *r, const vox_vtx *a, const vox_vtx *b,
                           const vox_vtx *c, const uint16_t *tile);

#ifdef __cplusplus
}
#endif