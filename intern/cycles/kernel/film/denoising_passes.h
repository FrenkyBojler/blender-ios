/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/closure/bsdf.h"

#include "kernel/film/write.h"

CCL_NAMESPACE_BEGIN

#ifdef __DENOISING_FEATURES__
ccl_device_forceinline void film_write_denoising_features_surface(KernelGlobals kg,
                                                                  IntegratorState state,
                                                                  const ccl_private ShaderData *sd,
                                                                  ccl_global float *ccl_restrict
                                                                      render_buffer)
{
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  if (!(path_flag & PATH_RAY_DENOISING_FEATURES)) {
    return;
  }

  /* Skip implicitly transparent surfaces. */
  if (sd->flag & SD_HAS_ONLY_VOLUME) {
    return;
  }

  /* Don't write denoising passes for paths that were split off for shadow catchers
   * to avoid double-counting. */
  if (path_flag & PATH_RAY_SHADOW_CATCHER_PASS) {
    return;
  }

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

  if (kernel_data.film.pass_denoising_depth != PASS_UNUSED) {
    const Spectrum denoising_feature_throughput = INTEGRATOR_STATE(
        state, path, denoising_feature_throughput);
    const float depth = sd->ray_length - INTEGRATOR_STATE(state, ray, tmin);
    const float denoising_depth = ensure_finite(average(denoising_feature_throughput) * depth);
    film_write_pass_float(buffer + kernel_data.film.pass_denoising_depth, denoising_depth);
  }

  float3 normal = zero_float3();
  Spectrum diffuse_albedo = zero_spectrum();
  Spectrum specular_albedo = zero_spectrum();
  float alpha = 0.0f;
  float sum_weight = 0.0f;
  float sum_nonspecular_weight = 0.0f;

  for (int i = 0; i < sd->num_closure; i++) {
    const ccl_private ShaderClosure *sc = &sd->closure[i];

    if (!CLOSURE_IS_BSDF_OR_BSSRDF(sc->type)) {
      continue;
    }

    /* All closures contribute to the normal feature, but only diffuse-like ones to the albedo. */
    /* If far-field hair, use fiber tangent as feature instead of normal. */
    normal += (sc->type == CLOSURE_BSDF_HAIR_HUANG_ID ? safe_normalize(sd->dPdu) : sc->N) *
              sc->sample_weight;
    sum_weight += sc->sample_weight;

    const Spectrum closure_albedo = bsdf_albedo(kg, sd, sc, true, true);
    const float closure_alpha = bsdf_get_specular_roughness_squared(sc);
    if (closure_alpha > sqr(0.075f) || sc->type == CLOSURE_BSDF_HAIR_HUANG_ID) {
      /* Far-field hair models "count" as diffuse. */
      diffuse_albedo += closure_albedo;
      sum_nonspecular_weight += sc->sample_weight;
    }
    else {
      specular_albedo += closure_albedo;
    }

    alpha += closure_alpha * sc->sample_weight;
  }

  if (sum_weight != 0.0f) {
    normal /= sum_weight;
    alpha /= sum_weight;
  }

  if (!(sd->flag & (SD_TRANSPARENT | SD_RAY_PORTAL)) &&
      kernel_data.film.pass_denoising_specular_albedo != PASS_UNUSED)
  {
    const Spectrum denoising_feature_throughput = INTEGRATOR_STATE(
        state, path, denoising_feature_throughput);

    /* Approximation of specular BRDF integral, see equation 4 in Ray Tracing Gems chapter 32. */
    const float alpha2 = alpha * alpha;
    const float alpha3 = alpha2 * alpha;
    const float omega = fabsf(dot(-sd->wi, normal));
    const float omega2 = omega * omega;
    const float omega3 = omega2 * omega;

    float bias = max(0.0f,
                     ((0.99044f + -1.28514f * omega) + (1.29678f + -0.755907f * omega) * alpha) /
                         ((1.0f + 2.92338f * omega + 59.4188f * omega3) +
                          (20.3225f + -27.0302f * omega + 222.592f * omega3) * alpha +
                          (121.563f + 626.13f * omega + 316.627f * omega3) * alpha3));
    float scale = max(0.0f,
                      ((0.0365463f + 3.32707f * omega) + (9.0632f + -9.04756f * omega) * alpha) /
                          ((1.0f + 3.59685f * omega2 + -1.36772f * omega3) +
                           (9.04401f + -16.3174f * omega2 + 9.22949f * omega3) * alpha +
                           (5.56589f + 19.7886f * omega2 + -20.2123f * omega3) * alpha3));

    /* This is a hack for specular reflectance of zero. */
    bias *= saturate(specular_albedo * 50).y;

    const Spectrum denoising_specular_albedo = ensure_finite(
        denoising_feature_throughput * (specular_albedo * scale + make_float3(bias)));
    film_write_pass_spectrum(buffer + kernel_data.film.pass_denoising_specular_albedo,
                             denoising_specular_albedo);
  }

  /* Wait for next bounce if 75% or more sample weight belongs to specular-like closures. */
  if ((sum_weight == 0.0f) || (sum_nonspecular_weight * 4.0f > sum_weight)) {
    if (kernel_data.film.pass_denoising_normal != PASS_UNUSED) {
      /* Transform normal into camera space. It should be transformed using the inverse transpose
       * of the transformation matrix, but since we ignore scaling for camera transformations, it's
       * equivalent to applying the transform directly. */
      const Transform worldtocamera = kernel_data.cam.worldtocamera;
      normal = transform_direction(&worldtocamera, normal);

      const float3 denoising_normal = ensure_finite(normal);
      film_write_pass_float3(buffer + kernel_data.film.pass_denoising_normal, denoising_normal);
    }

    if (kernel_data.film.pass_denoising_albedo != PASS_UNUSED) {
      const Spectrum denoising_feature_throughput = INTEGRATOR_STATE(
          state, path, denoising_feature_throughput);
      const Spectrum denoising_albedo = ensure_finite(denoising_feature_throughput *
                                                      diffuse_albedo);
      film_write_pass_spectrum(buffer + kernel_data.film.pass_denoising_albedo, denoising_albedo);
    }

    INTEGRATOR_STATE_WRITE(state, path, flag) &= ~PATH_RAY_DENOISING_FEATURES;
  }
  else {
    INTEGRATOR_STATE_WRITE(state, path, denoising_feature_throughput) *= specular_albedo;
  }
}

ccl_device_forceinline void film_write_denoising_features_volume(KernelGlobals kg,
                                                                 IntegratorState state,
                                                                 const Spectrum albedo,
                                                                 const bool scatter,
                                                                 ccl_global float *ccl_restrict
                                                                     render_buffer)
{
  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  const Spectrum denoising_feature_throughput = INTEGRATOR_STATE(
      state, path, denoising_feature_throughput);

  if (scatter && kernel_data.film.pass_denoising_normal != PASS_UNUSED) {
    /* Assume scatter is sufficiently diffuse to stop writing denoising features. */
    INTEGRATOR_STATE_WRITE(state, path, flag) &= ~PATH_RAY_DENOISING_FEATURES;

    /* Write view direction as normal. */
    const float3 denoising_normal = make_float3(0.0f, 0.0f, -1.0f);
    film_write_pass_float3(buffer + kernel_data.film.pass_denoising_normal, denoising_normal);
  }

  if (kernel_data.film.pass_denoising_albedo != PASS_UNUSED) {
    /* Write albedo. */
    const Spectrum denoising_albedo = ensure_finite(denoising_feature_throughput * albedo);
    film_write_pass_spectrum(buffer + kernel_data.film.pass_denoising_albedo, denoising_albedo);
  }
}

ccl_device_forceinline void film_write_denoising_features_background(
    KernelGlobals kg, IntegratorState state, ccl_global float *ccl_restrict render_buffer)
{
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  if (!(path_flag & PATH_RAY_DENOISING_FEATURES)) {
    return;
  }

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

  if (kernel_data.film.pass_denoising_depth != PASS_UNUSED) {
    film_write_pass_float(buffer + kernel_data.film.pass_denoising_depth, FLT_MAX);
  }

  if (kernel_data.film.pass_denoising_normal != PASS_UNUSED) {
    film_write_pass_float3(buffer + kernel_data.film.pass_denoising_normal, zero_float3());
  }

  /* 'pass_denoising_albedo' is written by 'film_write_emission_or_background_pass' */

  if (kernel_data.film.pass_denoising_specular_albedo != PASS_UNUSED) {
    film_write_pass_spectrum(buffer + kernel_data.film.pass_denoising_specular_albedo,
                             zero_float3());
  }
}
#endif /* __DENOISING_FEATURES__ */

CCL_NAMESPACE_END
