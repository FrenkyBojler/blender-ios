/* SPDX-FileCopyrightText: 2017-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/globals.h"

#include "kernel/film/write.h"

CCL_NAMESPACE_BEGIN

class gpu_kernel_timer {
 public:
  ccl_device_inline_method gpu_kernel_timer(ccl_global float *render_buffer, int state)
  {
    if (kernel_data.film.pass_render_time != PASS_UNUSED) {
      buffer = film_pass_pixel_render_buffer(nullptr, state, render_buffer) +
               kernel_data.film.pass_render_time;
      start_time = gpu_time_fast();
    }
    else {
      buffer = nullptr;
    }
  }
  ccl_device_inline_method ~gpu_kernel_timer()
  {
    if (buffer != nullptr) {
      film_write_pass_float(buffer, gpu_time_fast() - start_time);
    }
  }

 private:
  uint64_t start_time;
  ccl_global float *buffer;
};

CCL_NAMESPACE_END
