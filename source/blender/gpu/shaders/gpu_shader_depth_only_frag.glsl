/* SPDX-FileCopyrightText: 2016-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpu_shader_3D_depth_only_infos.hh"

/* WORKAROUND: gpu_shader_fullscreen_vert.glsl needs infos from all the engines that use it. */
#include "workbench_shadow_infos.hh"

void main()
{
  /* No color output, only depth (line below is implicit). */
  // gl_FragDepth = gl_FragCoord.z;
}
