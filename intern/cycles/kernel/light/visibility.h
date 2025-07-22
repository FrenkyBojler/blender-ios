/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2025 Blender Foundation */

#pragma once

CCL_NAMESPACE_BEGIN

// float EGB to RGBE conversion as described by Greg Ward
// http://www.graphics.cornell.edu/online/formats/rgbe/

ccl_device_inline SpectrumRGBE SpectrumToSpectrumRGBE(ccl_private const Spectrum *spec)
{
  SpectrumRGBE specRGBE;

  float v;
  int e;
  v = max(spec->x, max(spec->y, spec->z));
  if (v < 1e-32f) {
    specRGBE.r = specRGBE.g = specRGBE.b = specRGBE.e = 0;
  }
  else {
#if defined(__KERNEL_METAL__)
    v = frexp(v, e) * 256.0f / v;
#else
    v = frexp(v, &e) * 256.0f / v;
#endif
    specRGBE.r = (uint8_t)(spec->x * v);
    specRGBE.g = (uint8_t)(spec->y * v);
    specRGBE.b = (uint8_t)(spec->z * v);
    specRGBE.e = (uint8_t)(e + 128);
  }
  return specRGBE;
}

ccl_device_inline Spectrum SpectrumRGBEToSpectrum(ccl_private const SpectrumRGBE *specRGBE)
{
  Spectrum spec;

  if (specRGBE->e) {
    const float f = ldexpf(1.0f, specRGBE->e - (int)(128 + 8));
    spec.x = specRGBE->r * f;
    spec.y = specRGBE->g * f;
    spec.z = specRGBE->b * f;
  }
  else {
    spec.x = spec.y = spec.z = 0.0f;
  }
  return spec;
}

ccl_device_inline BsdfEvalRGBE BsdfEvalToBsdfEvalRGBE(ccl_private const BsdfEval *bsdfEval)
{
  BsdfEvalRGBE bsdfEvalRGBE;
  bsdfEvalRGBE.diffuse = SpectrumToSpectrumRGBE(&bsdfEval->diffuse);
  bsdfEvalRGBE.glossy = SpectrumToSpectrumRGBE(&bsdfEval->glossy);
  bsdfEvalRGBE.sum = SpectrumToSpectrumRGBE(&bsdfEval->sum);
  return bsdfEvalRGBE;
}

ccl_device_inline BsdfEval BsdfEvalRGBEToBsdfEval(ccl_private const BsdfEvalRGBE *bsdfEvalRGBE)
{
  BsdfEval bsdfEval;
  bsdfEval.diffuse = SpectrumRGBEToSpectrum(&bsdfEvalRGBE->diffuse);
  bsdfEval.glossy = SpectrumRGBEToSpectrum(&bsdfEvalRGBE->glossy);
  bsdfEval.sum = SpectrumRGBEToSpectrum(&bsdfEvalRGBE->sum);
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
  // We do not need to correct the light visibility for volume scattering events or camera rays
  if ((path_flag & PATH_RAY_VOLUME_SCATTER) || (path_flag & PATH_RAY_CAMERA)) {
    return one_spectrum();
  }

  Spectrum visible_components = zero_spectrum();
  const BsdfEvalRGBE scatter_eval_rgbe = INTEGRATOR_STATE(state, path, scatter_eval);
  const BsdfEval scatter_eval = BsdfEvalRGBEToBsdfEval(&scatter_eval_rgbe);
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
