/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_shader_to_rgba(Closure cl, float4 &outcol, float &outalpha)
{
#ifdef GPU_VERTEX_SHADER
  outcol = closure_to_rgba(cl);
#else
  outcol = float4(0.0f);
#endif
  outalpha = outcol.a;
}
