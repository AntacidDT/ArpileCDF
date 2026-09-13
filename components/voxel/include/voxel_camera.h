#pragma once

#include <stdbool.h>
#include "voxel_math.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    vox_vec3 pos;
    float yaw;
    float pitch;
    float fov_y;
    float near_plane;
    float far_plane;
} vox_camera;

typedef struct {
    vox_vec3 r;
    vox_vec3 u;
    vox_vec3 f;
} vox_camera_basis;

vox_camera_basis vox_camera_get_basis(const vox_camera *cam);

vox_vec3 vox_camera_world_to_view(const vox_camera *cam, vox_vec3 world);

typedef struct {
    float focal;
    float cx;
    float cy;
} vox_camera_proj;

vox_camera_proj vox_camera_make_proj(const vox_camera *cam, int vw, int vh);

bool vox_camera_project(const vox_camera_proj *p, vox_vec3 view,
                        float *sx, float *sy, float *inv_z);

#ifdef __cplusplus
}
#endif