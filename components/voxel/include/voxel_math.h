#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x, y, z;
} vox_vec3;

vox_vec3 vox_v3(float x, float y, float z);

vox_vec3 vox_v3_add(vox_vec3 a, vox_vec3 b);

vox_vec3 vox_v3_sub(vox_vec3 a, vox_vec3 b);

vox_vec3 vox_v3_scale(vox_vec3 a, float s);

float vox_v3_dot(vox_vec3 a, vox_vec3 b);

vox_vec3 vox_v3_cross(vox_vec3 a, vox_vec3 b);

vox_vec3 vox_v3_normalize(vox_vec3 a);

float vox_v3_length(vox_vec3 a);

vox_vec3 vox_rot_y(vox_vec3 v, float yaw);

vox_vec3 vox_rot_x(vox_vec3 v, float pitch);

float vox_clampf(float v, float lo, float hi);

#ifdef __cplusplus
}
#endif