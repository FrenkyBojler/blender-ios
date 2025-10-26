/* SPDX-FileCopyrightText: 2011-2025 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifdef __KERNEL_GPU__

#include "kernel/film/write.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline void write_render_time(IntegratorState state,
                                         ccl_global float *ccl_restrict render_buffer,
                                         uint64_t start_time)
{
  if (!render_buffer || kernel_data.film.pass_render_time == PASS_UNUSED) {
    return;
  }

  ccl_global float *buffer = film_pass_pixel_render_buffer(nullptr, state, render_buffer);
  film_write_pass_float(buffer + kernel_data.film.pass_render_time, gpu_time_fast() - start_time);
}

CCL_NAMESPACE_END

#endif
