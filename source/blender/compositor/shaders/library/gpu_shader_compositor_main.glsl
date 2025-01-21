/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* The texture loader utilities are needed to sample the input textures and initialize the
 * attributes. */
#include "gpu_shader_compositor_texture_utilities.glsl"

/* Add implementation for implicit conversion operations inserted by the code generator. This
 * file should include the functions [float|vec3|vec4]_from_[float|vec3|vec4]. */
#include "gpu_shader_compositor_type_conversion.glsl"

/* The compute shader that will be dispatched by the compositor ShaderOperation. It just calls the
 * evaluate function that will be dynamically generated and appended to this shader in the
 * ShaderOperation::generate_code method. */
void main()
{
  evaluate();
}
