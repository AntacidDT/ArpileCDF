#include <math.h>
#include "voxel_math.h"

vox_vec3 vox_v3(float x, float y, float z)
{
    vox_vec3 v = { x, y, z };
    return v;
}

vox_vec3 vox_v3_add(vox_vec3 a, vox_vec3 b)
{
    vox_vec3 v = { a.x + b.x, a.y + b.y, a.z + b.z };
    return v;
}

vox_vec3 vox_v3_sub(vox_vec3 a, vox_vec3 b)
{
    vox_vec3 v = { a.x - b.x, a.y - b.y, a.z - b.z };
    return v;
}

vox_vec3 vox_v3_scale(vox_vec3 a, float s)
{
    vox_vec3 v = { a.x * s, a.y * s, a.z * s };
    return v;
}

float vox_v3_dot(vox_vec3 a, vox_vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

vox_vec3 vox_v3_cross(vox_vec3 a, vox_vec3 b)
{
    vox_vec3 v = {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
    return v;
}

float vox_v3_length(vox_vec3 a)
{
    return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
}

vox_vec3 vox_v3_normalize(vox_vec3 a)
{
    float len = vox_v3_length(a);
    if (len < 1e-6f) {
        return vox_v3(0.0f, 0.0f, 0.0f);
    }
    return vox_v3_scale(a, 1.0f / len);
}

vox_vec3 vox_rot_y(vox_vec3 v, float yaw)
{
    float c = cosf(yaw);
    float s = sinf(yaw);
    vox_vec3 r = { v.x * c + v.z * s, v.y, -v.x * s + v.z * c };
    return r;
}

vox_vec3 vox_rot_x(vox_vec3 v, float pitch)
{
    float c = cosf(pitch);
    float s = sinf(pitch);
    vox_vec3 r = { v.x, v.y * c - v.z * s, v.y * s + v.z * c };
    return r;
}

float vox_clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}