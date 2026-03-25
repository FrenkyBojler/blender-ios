/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "FN_init.hh"
#include "FN_multi_function_builder.hh"
#include "FN_multi_function_registry.hh"

namespace blender::fn::multi_function {

void register_common_functions()
{
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>("sin(float)", [](const float a) { return sinf(a); });
  });
}

}  // namespace blender::fn::multi_function
