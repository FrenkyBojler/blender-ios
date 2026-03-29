/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/svm/mapping_util.h"
#include "kernel/svm/node_types.h"
#include "kernel/svm/util.h"

CCL_NAMESPACE_BEGIN

/* Mapping Node */

template<typename Float3Type>
ccl_device_noinline void svm_node_mapping(ccl_private float *ccl_restrict stack,
                                          const ccl_global SVMNodeMapping &ccl_restrict node)
{
  const float3 location = stack_load(stack, node.location);
  const float3 rotation = stack_load(stack, node.rotation);
  const float3 scale = stack_load(stack, node.scale);

  const Float3Type vector = stack_load<Float3Type>(stack, node.vector);
  const Float3Type result = svm_mapping(node.mapping_type, vector, location, rotation, scale);
  stack_store(stack, node.result_offset, result);
}

/* Texture Mapping */

ccl_device_noinline void svm_node_texture_mapping(
    ccl_private float *ccl_restrict stack,
    const ccl_global SVMNodeTextureMapping &ccl_restrict node)
{
  const float3 v = stack_load_float3(stack, node.vec_offset);

  Transform tfm;
  tfm.x = node.tfm_x;
  tfm.y = node.tfm_y;
  tfm.z = node.tfm_z;

  const float3 r = transform_point(&tfm, v);
  stack_store_float3(stack, node.out_offset, r);
}

ccl_device_noinline void svm_node_min_max(ccl_private float *ccl_restrict stack,
                                          const ccl_global SVMNodeMinMax &ccl_restrict node)
{
  const float3 v = stack_load_float3(stack, node.vec_offset);

  const float3 mn = make_float3(node.mn);
  const float3 mx = make_float3(node.mx);

  const float3 r = min(max(mn, v), mx);
  stack_store_float3(stack, node.out_offset, r);
}

CCL_NAMESPACE_END
