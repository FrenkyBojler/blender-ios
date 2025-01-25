/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/film/write.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline void film_write_aov_pass_value(KernelGlobals kg,
                                                 ConstIntegratorState state,
                                                 ccl_global float *ccl_restrict render_buffer,
                                                 const int aov_id,
                                                 const float value)
{
  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  film_write_pass_float(buffer + kernel_data.film.pass_aov_value + aov_id, value);
}

ccl_device_inline void film_write_aov_pass_color(KernelGlobals kg,
                                                 ConstIntegratorState state,
                                                 ccl_global float *ccl_restrict render_buffer,
                                                 const int aov_id,
                                                 const float3 color)
{
  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  film_write_pass_float4(buffer + kernel_data.film.pass_aov_color + aov_id,
                         make_float4(color, 1.0f));
}

ccl_device_inline void film_write_aov_pass_vector(KernelGlobals kg,
                                                  ConstIntegratorState state,
                                                  ccl_global float *ccl_restrict render_buffer,
                                                  const int aov_id,
                                                  const float3 vector)
{
  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  film_write_pass_float4(buffer + kernel_data.film.pass_aov_vector + aov_id,
                         make_float4(vector.x, vector.y, vector.z, 1.0f));
}

#ifdef __OSL__
ccl_device_inline AOVDescriptor aov_not_found()
{
  const AOVDescriptor desc = {(uint64_t)0ull, OUTPUT_AOV_TYPE_NONE, -1};
  return desc;
}

ccl_device_inline AOVDescriptor find_aov(KernelGlobals kg, uint64_t tag)
{
  if (tag == (uint64_t)0ull) {
    return aov_not_found();
  }

  uint aov_desc_offset = 0;
  AOVDescriptor aov_desc = kernel_data_fetch(aov_descs, aov_desc_offset);

  while (aov_desc.name != tag) {
    if (aov_desc.type == OUTPUT_AOV_TYPE_NONE) {
      /* end of the array */
      return aov_not_found();
    }
    aov_desc_offset += 1;
    aov_desc = kernel_data_fetch(aov_descs, aov_desc_offset);
  }

  return aov_desc;
}

ccl_device_inline void film_write_aov_pass(KernelGlobals kg,
                                           ConstIntegratorState state,
                                           ccl_global float *ccl_restrict render_buffer,
                                           const OutputAOVType type,
                                           const int offset,
                                           const float3 value)
{
  if (type == OUTPUT_AOV_TYPE_NONE) {
    return;
  }
  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  switch (type) {
    case OUTPUT_AOV_TYPE_VALUE:
      film_write_pass_float(buffer + kernel_data.film.pass_aov_value + offset, value.x);
      break;
    case OUTPUT_AOV_TYPE_COLOR:
      film_write_pass_float4(buffer + kernel_data.film.pass_aov_color + offset,
                             make_float4(value.x, value.y, value.z, 1.0f));
      break;
    case OUTPUT_AOV_TYPE_VECTOR:
      film_write_pass_float4(buffer + kernel_data.film.pass_aov_vector + offset,
                             make_float4(value.x, value.y, value.z, 1.0f));
      break;
    default:
      break;
  }
}
#endif

CCL_NAMESPACE_END
