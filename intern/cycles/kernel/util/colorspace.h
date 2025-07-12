/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/globals.h"
#include "kernel/tables.h"

#include "util/types_spectrum.h"

CCL_NAMESPACE_BEGIN

ccl_device float3 xyz_to_rgb(KernelGlobals kg, const float3 xyz)
{
  return make_float3(dot(make_float3(kernel_data.film.xyz_to_r), xyz),
                     dot(make_float3(kernel_data.film.xyz_to_g), xyz),
                     dot(make_float3(kernel_data.film.xyz_to_b), xyz));
}

ccl_device float3 xyz_to_rgb_clamped(KernelGlobals kg, const float3 xyz)
{
  return max(xyz_to_rgb(kg, xyz), zero_float3());
}

ccl_device float3 rec709_to_rgb(KernelGlobals kg, const float3 rec709)
{
  return (kernel_data.film.is_rec709) ?
             rec709 :
             make_float3(dot(make_float3(kernel_data.film.rec709_to_r), rec709),
                         dot(make_float3(kernel_data.film.rec709_to_g), rec709),
                         dot(make_float3(kernel_data.film.rec709_to_b), rec709));
}

ccl_device float linear_rgb_to_gray(KernelGlobals kg, const float3 c)
{
  return dot(c, make_float3(kernel_data.film.rgb_to_y));
}

ccl_device_inline Spectrum rgb_to_spectrum(const float3 rgb)
{
  return rgb;
}

ccl_device_inline float3 spectrum_to_rgb(Spectrum s)
{
  return s;
}

ccl_device float spectrum_to_gray(KernelGlobals kg, Spectrum c)
{
  return linear_rgb_to_gray(kg, spectrum_to_rgb(c));
}

ccl_device float3 wavelength_color_xyz(const float lambda_nm)
{
  float ii = (lambda_nm - 380.0f) * (1.0f / 5.0f);  // scaled 0..80
  const int i = float_to_int(ii);
  float3 color;

  if (i < 0 || i >= 80) {
    color = make_float3(0.0f, 0.0f, 0.0f);
  }
  else {
    ii -= i;
    ccl_constant float *c = cie_color_match[i];
    color = interp(make_float3(c[0], c[1], c[2]), make_float3(c[3], c[4], c[5]), ii);
  }

  return color;
}

CCL_NAMESPACE_END
