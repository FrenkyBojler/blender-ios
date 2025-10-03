/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_create_info.hh"
#include "gpu_shader_private.hh"

#include <sstream>

namespace blender::gpu {

uint32_t available_buffer_slots(const shader::ShaderCreateInfo &info);

std::pair<std::string, std::string> generate_entry_point(const shader::ShaderCreateInfo &info,
                                                         const ShaderStage stage,
                                                         const StringRefNull entry_point_name);

void patch_create_info_atomic_workaround(std::unique_ptr<shader::ShaderCreateInfo> &patched_info,
                                         shader::ShaderCreateInfoStringCache &patched_names,
                                         const shader::ShaderCreateInfo &original_info);

}  // namespace blender::gpu
