#include <stdlib.h>
#include <string.h>
#include "voxel_world.h"
#include "voxel_generator.h"
#include "voxel_block.h"
#include "voxel_mesh.h"
#include "voxel_texture.h"
#include "esp_heap_caps.h"

struct vox_world {
    uint32_t seed;
    int cx, cy, cz;            /* chunks per axis */
    vox_chunk *chunks;         /* flat array, Y-major then Z then X */
    vox_mesh_face *faces;      /* persistent face mesh */
    int face_count;
    int face_cap;
};

static int chunk_index(const vox_world *w, int x, int y, int z)
{
    return (x * w->cy + y) * w->cz + z;
}

vox_world *vox_world_create(uint32_t seed, int chunk_x, int chunk_y, int chunk_z)
{
    if (chunk_x < 1) chunk_x = 1;
    if (chunk_y < 1) chunk_y = 1;
    if (chunk_z < 1) chunk_z = 1;

    vox_world *w = calloc(1, sizeof(vox_world));
    if (!w) return NULL;
    w->seed = seed;
    w->cx = chunk_x;
    w->cy = chunk_y;
    w->cz = chunk_z;

    size_t n = (size_t)chunk_x * chunk_y * chunk_z;
    if (n == 0) {
        free(w);
        return NULL;
    }

    w->chunks = heap_caps_malloc(n * sizeof(vox_chunk), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!w->chunks) {
        w->chunks = calloc(n, sizeof(vox_chunk));
    }
    if (!w->chunks) {
        free(w);
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        memset((void *)w->chunks[i].blocks, VOX_BLOCK_AIR, sizeof(w->chunks[i].blocks));
    }
    return w;
}

void vox_world_destroy(vox_world *w)
{
    if (!w) return;
    if (w->chunks) heap_caps_free(w->chunks);
    if (w->faces) heap_caps_free(w->faces);
    free(w);
}

void vox_world_generate(vox_world *w)
{
    if (!w) return;
    vox_gen_world(w->seed, w);
    vox_world_build_mesh(w);
}

uint8_t vox_world_get(const vox_world *w, int wx, int wy, int wz)
{
    if (!w || wx < 0 || wy < 0 || wz < 0) return VOX_BLOCK_AIR;
    int ccx = wx >> 4;
    int ccy = wy >> 4;
    int ccz = wz >> 4;
    if (ccx >= w->cx || ccy >= w->cy || ccz >= w->cz) return VOX_BLOCK_AIR;
    return vox_chunk_get(&w->chunks[chunk_index(w, ccx, ccy, ccz)],
                         wx & 15, wy & 15, wz & 15);
}

void vox_world_set(vox_world *w, int wx, int wy, int wz, uint8_t block)
{
    if (!w || wx < 0 || wy < 0 || wz < 0) return;
    int ccx = wx >> 4;
    int ccy = wy >> 4;
    int ccz = wz >> 4;
    if (ccx >= w->cx || ccy >= w->cy || ccz >= w->cz) return;
    vox_chunk_set(&w->chunks[chunk_index(w, ccx, ccy, ccz)],
                  wx & 15, wy & 15, wz & 15, block);
}

uint32_t vox_world_seed(const vox_world *w)
{
    return w ? w->seed : 0;
}

int vox_world_chunks_x(const vox_world *w) { return w ? w->cx : 0; }
int vox_world_chunks_y(const vox_world *w) { return w ? w->cy : 0; }
int vox_world_chunks_z(const vox_world *w) { return w ? w->cz : 0; }

int vox_world_block_count(const vox_world *w)
{
    if (!w) return 0;
    int n = 0;
    size_t count = (size_t)w->cx * w->cy * w->cz;
    for (size_t i = 0; i < count; i++) {
        n += vox_chunk_block_count(&w->chunks[i]);
    }
    return n;
}

static void face_reserve(vox_world *w, int extra)
{
    if (w->face_count + extra > w->face_cap) {
        int cap = w->face_cap ? w->face_cap * 2 : 4096;
        while (cap < w->face_count + extra) cap *= 2;
        vox_mesh_face *nf = heap_caps_malloc((size_t)cap * sizeof(vox_mesh_face),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nf) {
            nf = malloc((size_t)cap * sizeof(vox_mesh_face));
        }
        if (!nf) return;
        if (w->face_count > 0) {
            memcpy(nf, w->faces, (size_t)w->face_count * sizeof(vox_mesh_face));
        }
        if (w->faces) heap_caps_free(w->faces);
        w->faces = nf;
        w->face_cap = cap;
    }
}

/* Build the persistent face list: only faces whose outward neighbour is AIR
 * (or outside the world) are emitted. */
void vox_world_build_mesh(vox_world *w)
{
    if (!w) return;
    if (w->faces) {
        heap_caps_free(w->faces);
        w->faces = NULL;
        w->face_count = 0;
        w->face_cap = 0;
    }

    int wx0 = 0, wy0 = 0, wz0 = 0;
    int wx1 = w->cx * 16, wy1 = w->cy * 16, wz1 = w->cz * 16;

    for (int wy = wy0; wy < wy1; wy++) {
        for (int wz = wz0; wz < wz1; wz++) {
            for (int wx = wx0; wx < wx1; wx++) {
                uint8_t b = vox_world_get(w, wx, wy, wz);
                if (b == VOX_BLOCK_AIR) continue;
                if (!vox_block_is_opaque(b)) continue;

                for (int d = 0; d < VOX_FACE_COUNT; d++) {
                    int nx = wx + vox_mesh_delta[d][0];
                    int ny = wy + vox_mesh_delta[d][1];
                    int nz = wz + vox_mesh_delta[d][2];
                    uint8_t nb = vox_world_get(w, nx, ny, nz);
                    if (nb != VOX_BLOCK_AIR && vox_block_is_opaque(nb)) continue;

                    face_reserve(w, 1);
                    vox_mesh_face *f = &w->faces[w->face_count];
                    f->x = (int16_t)wx;
                    f->y = (int16_t)wy;
                    f->z = (int16_t)wz;
                    f->dir = (uint8_t)d;
                    f->tex = (uint8_t)vox_block_face_tex(b, d);
                    w->face_count++;
                }
            }
        }
    }
}

int vox_world_face_count(const vox_world *w)
{
    return w ? w->face_count : 0;
}

const vox_mesh_face *vox_world_faces(const vox_world *w)
{
    return w ? w->faces : NULL;
}