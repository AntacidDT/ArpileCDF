#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "voxel_generator.h"
#include "voxel_world.h"
#include "voxel_block.h"

#define SEED_SALT_TERRAIN 0x9E3779B9u
#define SEED_SALT_DETAIL  0x85EBCA6Bu
#define SEED_SALT_TREE    0xC2B2AE35u

static uint32_t hash2(uint32_t seed, int x, int z)
{
    uint32_t h = seed;
    h = (h ^ (uint32_t)x * 0x9E3779B9u) * 0x85EBCA6Bu;
    h = (h ^ (uint32_t)z * 0xC2B2AE35u) * 0x27D4EB0Fu;
    h ^= h >> 15;
    h *= 0x165667B1u;
    h ^= h >> 13;
    return h;
}

static float lattice(uint32_t seed, int x, int z)
{
    return (float)(hash2(seed, x, z) & 0xFFFFu) / 65535.0f;
}

static float smoothstep_v(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

/* Bilinear value noise on integer lattice positions. */
static float value_noise(uint32_t seed, float x, float z)
{
    int x0 = (int)floorf(x);
    int z0 = (int)floorf(z);
    float tx = smoothstep_v(x - (float)x0);
    float tz = smoothstep_v(z - (float)z0);

    float v00 = lattice(seed, x0, z0);
    float v10 = lattice(seed, x0 + 1, z0);
    float v01 = lattice(seed, x0, z0 + 1);
    float v11 = lattice(seed, x0 + 1, z0 + 1);

    float top = v00 + (v10 - v00) * tx;
    float bot = v01 + (v11 - v01) * tx;
    return top + (bot - top) * tz;
}

/* Two-octave fbm in [0, 1]. */
static float fbm(uint32_t seed, float x, float z)
{
    float n = value_noise(seed, x, z);
    n += 0.5f * value_noise(seed ^ 0xA5C3E5u, x * 2.7f, z * 2.7f);
    return n / 1.5f;
}

int vox_gen_height(uint32_t seed, int x, int z)
{
    float n = fbm(seed ^ SEED_SALT_TERRAIN, x * 0.055f, z * 0.055f);
    float h = 3.0f + n * 12.0f;
    if (h < 2.0f) h = 2.0f;
    if (h > 14.0f) h = 14.0f;
    return (int)h;
}

static bool tree_site(uint32_t seed, int x, int z)
{
    return (hash2(seed ^ SEED_SALT_TREE, x, z) % 89u) == 0u;
}

/* Place a small oak on the column if the surface is grass and there is room. */
static void place_tree(uint32_t seed, vox_world *w, int x, int z)
{
    (void)seed;
    int h = vox_gen_height(seed, x, z);
    int y = h;

    /* Surface must be grass and the trunk must fit under the world top. */
    if (vox_world_get(w, x, y, z) != VOX_BLOCK_GRASS) return;
    if (y + 6 > 16) return;

    for (int k = 1; k <= 4; k++) {
        vox_world_set(w, x, y + k, z, VOX_BLOCK_WOOD);
    }
    /* Leaf blob: 5x5x3 with hollow cross shape + a cap ring. */
    int ly = y + 4;
    for (int dy = 0; dy <= 2; dy++) {
        int r = (dy == 0) ? 1 : 2;   /* dense near the top, wider below */
        for (int dx = -r; dx <= r; dx++) {
            for (int dz = -r; dz <= r; dz++) {
                if (dx == 0 && dz == 0 && dy < 2) continue;   /* trunk */
                if (dx == 0 && dz == 0) continue;

                int px = x + dx;
                int pz = z + dz;
                if (vox_world_get(w, px, ly + dy, pz) == VOX_BLOCK_AIR) {
                    vox_world_set(w, px, ly + dy, pz, VOX_BLOCK_LEAVES);
                }
            }
        }
    }
}

void vox_gen_world(uint32_t seed, vox_world *world)
{
    int cx = vox_world_chunks_x(world);
    int cy = vox_world_chunks_y(world);
    int cz = vox_world_chunks_z(world);

    for (int ccx = 0; ccx < cx; ccx++) {
        for (int ccz = 0; ccz < cz; ccz++) {
            for (int local_z = 0; local_z < 16; local_z++) {
                for (int local_x = 0; local_x < 16; local_x++) {
                    int wx = ccx * 16 + local_x;
                    int wz = ccz * 16 + local_z;
                    int h = vox_gen_height(seed, wx, wz);
                    int surface = (h <= 4) ? VOX_BLOCK_SAND : VOX_BLOCK_GRASS;

                    for (int wy = 0; wy < cy * 16; wy++) {
                        uint8_t b;
                        if (wy > h) {
                            b = VOX_BLOCK_AIR;
                        } else if (wy == h) {
                            b = (uint8_t)surface;
                        } else if (wy >= h - 3) {
                            b = (uint8_t)((surface == VOX_BLOCK_SAND) ? VOX_BLOCK_SAND : VOX_BLOCK_DIRT);
                        } else {
                            b = VOX_BLOCK_STONE;
                        }
                        vox_world_set(world, wx, wy, wz, b);
                    }
                }
            }
        }
    }

    /* Deterministic trees on top of the generated terrain. */
    for (int ccx = 0; ccx < cx; ccx++) {
        for (int ccz = 0; ccz < cz; ccz++) {
            for (int local_z = 0; local_z < 16; local_z++) {
                for (int local_x = 0; local_x < 16; local_x++) {
                    int wx = ccx * 16 + local_x;
                    int wz = ccz * 16 + local_z;
                    if (tree_site(seed, wx, wz)) {
                        place_tree(seed, world, wx, wz);
                    }
                }
            }
        }
    }
}