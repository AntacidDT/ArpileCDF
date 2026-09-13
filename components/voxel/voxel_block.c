#include "voxel_block.h"

static vox_tex_id tex_for(vox_block_id b, vox_tex_id top, vox_tex_id side, vox_tex_id bottom, int face)
{
    switch (face) {
    case VOX_FACE_Y_POS: return top;
    case VOX_FACE_Y_NEG: return bottom;
    default:             return side;
    }
}

vox_tex_id vox_block_face_tex(vox_block_id block, int face)
{
    switch (block) {
    case VOX_BLOCK_GRASS:
        if (face == VOX_FACE_Y_POS) return VOX_TEX_GRASS;
        if (face == VOX_FACE_Y_NEG) return VOX_TEX_DIRT;
        return VOX_TEX_GRASS_SIDE;
    case VOX_BLOCK_DIRT:
        return VOX_TEX_DIRT;
    case VOX_BLOCK_STONE:
        return VOX_TEX_STONE;
    case VOX_BLOCK_SAND:
        return VOX_TEX_SAND;
    case VOX_BLOCK_WOOD:
        return tex_for(block, VOX_TEX_WOOD_TOP, VOX_TEX_WOOD_SIDE, VOX_TEX_WOOD_TOP, face);
    case VOX_BLOCK_LEAVES:
        return VOX_TEX_LEAVES;
    default:
        return VOX_TEX_STONE;
    }
}

bool vox_block_is_opaque(vox_block_id block)
{
    return block != VOX_BLOCK_AIR;
}