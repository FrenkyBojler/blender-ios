/* SPDX-FileCopyrightText: 2016-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Preprocessor outputs this for GLSL compatibility. */
#ifndef SRD_IMPL_VertexIn
#  define SRD_IMPL_VertexIn DUMMY
#endif
#ifndef SRD_IMPL_VertexOut
#  define SRD_IMPL_VertexOut DUMMY
#endif
#ifndef SRD_IMPL_FragmentOut
#  define SRD_IMPL_FragmentOut DUMMY
#endif
#ifndef SRD_IMPL_ImageCommon
#  define SRD_IMPL_ImageCommon DUMMY
#endif

#define SRD_DECLARE_ImageCommon() SRD_DECLARE_SAMPLER(ImageCommon, 0, sampler2D, image)

/* Start of file. */

#include "gpu_shader_srd_cpp.hh"

SRD_VERTEX_IN_BEGIN(VertexIn)
SRD_VERTEX_IN(VertexIn, 0, float2, pos)
SRD_VERTEX_IN(VertexIn, 1, float2, uv)
SRD_VERTEX_IN_END(VertexIn)

SRD_VERTEX_OUT_BEGIN(VertexOut)
SRD_VERTEX_OUT(VertexOut, position, float4, position)
SRD_VERTEX_OUT(VertexOut, smooth, float2, uv)
SRD_VERTEX_OUT_END(VertexOut)

SRD_FRAGMENT_OUT_BEGIN(FragmentOut)
SRD_FRAGMENT_OUT(FragmentOut, 0, float4, fragColor)
SRD_FRAGMENT_OUT_END(FragmentOut)

SRD_RESOURCE_BEGIN(ImageCommon)
SRD_RESOURCE_PUSH_CONSTANT(ImageCommon, float4x4, ModelViewProjectionMatrix)
SRD_RESOURCE_SAMPLER(ImageCommon, 0, sampler2D, image)
SRD_RESOURCE_END(ImageCommon)

/* Shader code is not wanted when this file is included for shader reflections.
 * Guard all usage explicitly. This way, this can be included in many places without too much
 * overhead. This also allow arbitrary placement of the code w.r.t. the SRD. */
#ifndef NO_SHADER_CODE

VertexOut image_vertex(VertexIn v_in, ImageCommon srd)
{
  VertexOut v_out;
  v_out.position = srd.ModelViewProjectionMatrix * float4(v_in.pos.xy, 0.0f, 1.0f);
  v_out.uv = v_in.uv;
  return v_out;
}

FragmentOut image_fragment(VertexOut f_in, ImageCommon srd)
{
  FragmentOut f_out;
  f_out.fragColor = texture(srd.image, f_in.uv);
  return f_out;
}

#endif

SRD_GRAPHIC_PIPELINE(__FILE__,
                     ImageSimple,
                     VertexIn,
                     VertexOut,
                     VertexOut,
                     FragmentOut,
                     image_vertex,
                     ImageCommon,
                     image_fragment,
                     ImageCommon)
