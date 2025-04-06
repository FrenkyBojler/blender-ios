/* SPDX-FileCopyrightText: 2016-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef SHADER_REFLECTION
#  include "gpu_shader_reflection.hh"
#else
#  include "gpu_shader_compat.hh"
#  include "gpu_shader_compat_glsl.hh"
#endif

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

/* Preprocessor outputs this for GLSL compatibility. */
#ifdef GPU_LANG_GLSL
#  if SRD_ENABLED(ImageCommon)
SRD_SAMPLER_DECLARE(ImageCommon, 0, sampler2D, image)
#  else
SRD_SAMPLER_DECLARE_DUMMY(ImageCommon, 0, sampler2D, image)
#  endif
#endif

/* Shader code is not wanted when this file is included for shader reflections.
 * Guard all usage explicitly. This way, this can be included in many places without too much
 * overhead. This also allow arbitrary placement of the code w.r.t. the SRD. */
#ifndef NO_SHADER_CODE

VertexOut image_vertex(VertexIn in, ImageCommon srd)
{
  VertexOut out;
  out.position = srd.ModelViewProjectionMatrix * float4(in.pos.xy, 0.0f, 1.0f);
  out.uv = in.uv;
  return out;
}

FragmentOut image_fragment(VertexOut in, ImageCommon srd)
{
  FragmentOut out;
  out.fragColor = texture(srd.image, in.uv);
  return out;
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
