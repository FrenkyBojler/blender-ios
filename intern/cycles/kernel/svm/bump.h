/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/globals.h"

#include "kernel/geom/attribute.h"
#include "kernel/geom/object.h"
#include "kernel/geom/primitive.h"

#include "kernel/svm/util.h"

#include "kernel/util/differential.h"

CCL_NAMESPACE_BEGIN

/* Bump Eval Nodes */

ccl_device_noinline void svm_node_enter_bump_eval(KernelGlobals kg,
                                                  ccl_private ShaderData *sd,
                                                  ccl_private float *stack,
                                                  const uint offset)
{
  /* save state */
  stack_store_float3(stack, offset + 0, sd->P);
  stack_store_float(stack, offset + 3, sd->dP);

  /* Set position as if undisplaced. */
  const AttributeDescriptor desc = find_attribute(kg, sd, ATTR_STD_POSITION_UNDISPLACED);

  if (desc.offset != ATTR_STD_NOT_FOUND) {
    dual3 P = primitive_surface_attribute<float3>(kg, sd, desc, true, true);

    object_position_transform(kg, sd, &P);

    sd->P = P.val;
    sd->dP = differential_make_compact(P);

    /* Save the full differential, the compact form isn't enough for svm_node_set_bump. */
    stack_store_float3(stack, offset + 4, P.dx);
    stack_store_float3(stack, offset + 7, P.dy);
  }

  /* Set normal as if undisplaced.
   * Note this does not need to be restored, because the bump evaluation will
   * write to sd->N. */
  const AttributeDescriptor ndesc = find_attribute(kg, sd, ATTR_STD_NORMAL_UNDISPLACED);
  if (ndesc.offset != ATTR_STD_NOT_FOUND) {
    float3 N = safe_normalize(
        primitive_surface_attribute<float3>(kg, sd, ndesc, false, false).val);
    object_normal_transform(kg, sd, &N);
    sd->N = (sd->flag & SD_BACKFACING) ? -N : N;
  }
}

ccl_device_noinline void svm_node_leave_bump_eval(ccl_private ShaderData *sd,
                                                  ccl_private float *stack,
                                                  const uint offset)
{
  /* restore state */
  sd->P = stack_load_float3(stack, offset + 0);
  sd->dP = stack_load_float(stack, offset + 3);
}

CCL_NAMESPACE_END
