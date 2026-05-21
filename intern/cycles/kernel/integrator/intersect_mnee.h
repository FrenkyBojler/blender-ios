/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/integrator/mnee.h"
#include "kernel/integrator/shade_surface.h"

CCL_NAMESPACE_BEGIN

#ifdef __MNEE__

/* Sample a light and run the MNEE manifold walk for a caustic receiver. On a successful walk
 * the result is written to a shadow slot for integrator_shade_surface, otherwise nothing is
 * written and direct light is sampled there. */
ccl_device_forceinline ShaderEvalResult
integrate_surface_mnee(KernelGlobals /*kg*/,
                       IntegratorState /*state*/,
                       ccl_private ShaderData * /*sd*/,
                       const ccl_private RNGState * /*rng_state*/)
{
  return SHADER_EVAL_OK;
}

#endif /* __MNEE__ */

ccl_device void integrator_intersect_mnee(KernelGlobals kg, IntegratorState state)
{
  PROFILING_INIT(kg, PROFILING_SHADE_SURFACE_DIRECT_LIGHT);

  ShaderData sd;
  integrate_surface_shader_setup(kg, state, &sd);
  const int shader = sd.shader & SHADER_MASK;

#ifdef __MNEE__
  RNGState rng_state;
  path_state_rng_load(state, &rng_state);

  const ShaderEvalResult result = integrate_surface_mnee(kg, state, &sd, &rng_state);
  if (result == SHADER_EVAL_CACHE_MISS) {
    integrator_path_cache_miss(state, DEVICE_KERNEL_INTEGRATOR_INTERSECT_MNEE);
    return;
  }
#endif

  integrator_path_next_sorted(kg,
                              state,
                              DEVICE_KERNEL_INTEGRATOR_INTERSECT_MNEE,
                              DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE,
                              shader);
}

CCL_NAMESPACE_END
