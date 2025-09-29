/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_create_info.hh"
#include "gpu_shader_private.hh"

#include <sstream>

namespace blender::gpu {

uint32_t vertex_stage_available_slots(const shader::ShaderCreateInfo &info,
                                      const bool use_sampler_argument_buffer);

std::string generate_entry_point(const shader::ShaderCreateInfo &info,
                                 const ShaderStage stage,
                                 const StringRefNull entry_point_name);

}  // namespace blender::gpu
