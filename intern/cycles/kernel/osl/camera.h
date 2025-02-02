/* SPDX-FileCopyrightText: 2009-2010 Sony Pictures Imageworks Inc., et al. All Rights Reserved.
 * SPDX-FileCopyrightText: 2011-2024 Blender Foundation
 *
 * SPDX-License-Identifier: BSD-3-Clause */

#pragma once

#include "kernel/osl/types.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline void cameradata_to_shaderglobals(const packed_float3 sensor,
                                                   const packed_float3 dSdx,
                                                   const packed_float3 dSdy,
                                                   const float2 rand_lens,
                                                   ccl_private ShaderGlobals *globals)
{
  memset(globals, 0, sizeof(ShaderGlobals));

  globals->P = sensor;
  globals->dPdx = dSdx;
  globals->dPdy = dSdy;
  globals->N = make_float3(rand_lens);
}

#ifndef __KERNEL_GPU__

packed_float3 osl_eval_camera(KernelGlobals kg,
                              const packed_float3 sensor,
                              const packed_float3 dSdx,
                              const packed_float3 dSdy,
                              const float2 rand_lens,
                              packed_float3 &P,
                              packed_float3 &dPdx,
                              packed_float3 &dPdy,
                              packed_float3 &D,
                              packed_float3 &dDdx,
                              packed_float3 &dDdy);

#else

ccl_device_inline packed_float3 osl_eval_camera(KernelGlobals kg,
                                                const packed_float3 sensor,
                                                const packed_float3 dSdx,
                                                const packed_float3 dSdy,
                                                const float2 rand_lens,
                                                packed_float3 &P,
                                                packed_float3 &dPdx,
                                                packed_float3 &dPdy,
                                                packed_float3 &D,
                                                packed_float3 &dDdx,
                                                packed_float3 &dDdy)
{
  return zero_spectrum();
}

#endif

CCL_NAMESPACE_END
