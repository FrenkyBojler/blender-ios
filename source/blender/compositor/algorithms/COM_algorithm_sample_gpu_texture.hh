/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_vector_types.hh"

#include "COM_context.hh"
#include "COM_result.hh"

namespace blender::compositor {

/* Samples a pixel from a GPU texture. */
template<typename T>
T sample_gpu_texture(Context &context, const Result &result, const int2 texel);

}  // namespace blender::compositor
