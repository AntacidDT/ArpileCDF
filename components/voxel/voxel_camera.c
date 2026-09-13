#include <math.h>
#include "voxel_camera.h"
#include "voxel_math.h"

vox_camera_basis vox_camera_get_basis(const vox_camera *cam)
{
    float cy = cosf(cam->yaw);
    float sy = sinf(cam->yaw);
    float cp = cosf(cam->pitch);
    float sp = sinf(cam->pitch);
    vox_camera_basis b;
    b.f = vox_v3(sy * cp, sp, cy * cp);
    b.r = vox_v3(cy, 0.0f, -sy);
    b.u = vox_v3_cross(b.f, b.r);
    return b;
}

vox_vec3 vox_camera_world_to_view(const vox_camera *cam, vox_vec3 world)
{
    vox_camera_basis b = vox_camera_get_basis(cam);
    vox_vec3 d = vox_v3_sub(world, cam->pos);
    vox_vec3 v;
    v.x = vox_v3_dot(d, b.r);
    v.y = vox_v3_dot(d, b.u);
    v.z = vox_v3_dot(d, b.f);
    return v;
}

vox_camera_proj vox_camera_make_proj(const vox_camera *cam, int vw, int vh)
{
    vox_camera_proj p;
    p.focal = ((float)vh * 0.5f) / tanf(cam->fov_y * 0.5f);
    p.cx = (float)vw * 0.5f;
    p.cy = (float)vh * 0.5f;
    return p;
}

bool vox_camera_project(const vox_camera_proj *p, vox_vec3 view,
                        float *sx, float *sy, float *inv_z)
{
    if (view.z < 1e-4f) {
        return false;
    }
    float iz = 1.0f / view.z;
    *sx = p->cx + view.x * p->focal * iz;
    *sy = p->cy - view.y * p->focal * iz;
    *inv_z = iz;
    return true;
}