/* SPDX-FileCopyrightText: 2025 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Shared functions between OSL on CPU and GPU. */

#pragma once

#include "kernel/geom/motion_triangle.h"
#include "kernel/geom/triangle.h"

CCL_NAMESPACE_BEGIN

/* TODO: deduplicate function `set_attribute_float3()` in CPU and GPU. */

bool attribute_bump_map_normal(KernelGlobals kg, ccl_private const ShaderData *sd, float3 f[3])
{
  if (!(sd->type & PRIMITIVE_TRIANGLE) || !(sd->shader & SHADER_SMOOTH_NORMAL)) {
    return false;
  }

  if (sd->type == PRIMITIVE_TRIANGLE) {
    f[0] = sd->N;
    f[1] = triangle_smooth_normal(kg, sd->Ng, sd->prim, sd->u + sd->du.dx, sd->v + sd->dv.dx);
    f[2] = triangle_smooth_normal(kg, sd->Ng, sd->prim, sd->u + sd->du.dy, sd->v + sd->dv.dy);
  }
  else {
    f[0] = motion_triangle_smooth_normal(
        kg, sd->Ng, sd->object, sd->prim, sd->time, sd->u, sd->v, sd->du, sd->dv, f[1], f[2]);
  }

  if (sd->shader & SD_BACKFACING) {
    f[1] = -f[1];
    f[2] = -f[2];
  }
  f[1] -= f[0];
  f[2] -= f[0];

  return true;
}

CCL_NAMESPACE_END
