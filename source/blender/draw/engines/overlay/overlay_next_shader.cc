/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#include "overlay_next_private.hh"

namespace blender::draw::overlay {

ShaderModule *ShaderModule::g_shader_modules[2][2] = {{nullptr}};

StaticShader ShaderModule::shader_clippable(const char *create_info_name)
{
  std::string name = create_info_name;

  if (clipping_enabled_) {
    name += "_clipped";
  }

  return StaticShader(name);
}

StaticShader ShaderModule::shader_selectable(const char *create_info_name)
{
  std::string name = create_info_name;

  if (selection_type_ != SelectionType::DISABLED) {
    name += "_selectable";
  }

  if (clipping_enabled_) {
    name += "_clipped";
  }

  return StaticShader(name);
}

StaticShader ShaderModule::shader_selectable_no_clip(const char *create_info_name)
{
  std::string name = create_info_name;

  if (selection_type_ != SelectionType::DISABLED) {
    name += "_selectable";
  }

  return StaticShader(name);
}

using namespace blender::gpu::shader;

ShaderModule &ShaderModule::module_get(SelectionType selection_type, bool clipping_enabled)
{
  static std::mutex mutex;

  int selection_index = selection_type == SelectionType::DISABLED ? 0 : 1;
  ShaderModule *&g_shader_module = g_shader_modules[selection_index][clipping_enabled];
  if (g_shader_module == nullptr) {
    std::lock_guard lock(mutex);
    /* Check again in case it was created between first check and lock. */
    if (g_shader_module == nullptr) {
      g_shader_module = new ShaderModule(selection_type, clipping_enabled);
    }
  }
  return *g_shader_module;
}

void ShaderModule::module_free()
{
  for (int i : IndexRange(2)) {
    for (int j : IndexRange(2)) {
      if (g_shader_modules[i][j] != nullptr) {
        /* TODO(@fclem) thread-safety. */
        delete g_shader_modules[i][j];
        g_shader_modules[i][j] = nullptr;
      }
    }
  }
}

}  // namespace blender::draw::overlay
