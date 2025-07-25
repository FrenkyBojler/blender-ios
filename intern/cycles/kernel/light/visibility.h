/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2025 Blender Foundation */

#pragma once

CCL_NAMESPACE_BEGIN


ccl_device_inline BsdfEvalRGBE BsdfEvalToBsdfEvalRGBE(ccl_private const BsdfEval &bsdfEval)
{
  BsdfEvalRGBE bsdfEvalRGBE;
  bsdfEvalRGBE.diffuse = rgb_to_rgbe(
      make_float3(bsdfEval.diffuse.x, bsdfEval.diffuse.y, bsdfEval.diffuse.z));
  bsdfEvalRGBE.glossy = rgb_to_rgbe(
      make_float3(bsdfEval.glossy.x, bsdfEval.glossy.y, bsdfEval.glossy.z));
  bsdfEvalRGBE.sum = rgb_to_rgbe(make_float3(bsdfEval.sum.x, bsdfEval.sum.y, bsdfEval.sum.z));
  return bsdfEvalRGBE;
}

ccl_device_inline BsdfEval BsdfEvalRGBEToBsdfEval(ccl_private const BsdfEvalRGBE &bsdfEvalRGBE)
{
  BsdfEval bsdfEval;
  bsdfEval.diffuse = rgbe_to_rgb(bsdfEvalRGBE.diffuse);
  bsdfEval.glossy = rgbe_to_rgb(bsdfEvalRGBE.glossy);
  bsdfEval.sum = rgbe_to_rgb(bsdfEvalRGBE.sum);
  return bsdfEval;
}

/* Path tracing: When a light is hit by a forward path the path throughput weight already contains
  the product with the BSDF evaluation of all BSDF components (e.g., diffuse, glossy,
  transmission). To consider the light visibility correctly we need to calculate a correction
  factor that removes the contributions from invisible BSDF components from the paths throughput
  weight. Alternatively this factor can be multiplied to the light contribution. */
ccl_device_inline Spectrum light_visibility_correction(IntegratorState state,
                                                       ccl_private const int shader,
                                                       const uint32_t path_flag)
{
  /* We do not need to correct the light visibility for volume scattering events or camera rays */
  if ((path_flag & PATH_RAY_VOLUME_SCATTER) || (path_flag & PATH_RAY_CAMERA)) {
    return one_spectrum();
  }

  Spectrum visible_components = zero_spectrum();
  const BsdfEvalRGBE scatter_eval_rgbe = INTEGRATOR_STATE(state, path, scatter_eval);
  const BsdfEval scatter_eval = BsdfEvalRGBEToBsdfEval(scatter_eval_rgbe);
  Spectrum transmission = scatter_eval.sum - (scatter_eval.glossy + scatter_eval.diffuse);
  if (!(shader & SHADER_EXCLUDE_DIFFUSE)) {
    visible_components += scatter_eval.diffuse;
  }

  if (!(shader & SHADER_EXCLUDE_GLOSSY)) {
    visible_components += scatter_eval.glossy;
  }

  if (!(shader & SHADER_EXCLUDE_TRANSMIT)) {
    visible_components += transmission;
  }

  return safe_divide(visible_components, scatter_eval.sum);
}

CCL_NAMESPACE_END
