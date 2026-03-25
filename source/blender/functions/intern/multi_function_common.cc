/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "FN_init.hh"
#include "FN_multi_function_builder.hh"
#include "FN_multi_function_registry.hh"

namespace blender::fn::multi_function {

void register_common_functions()
{
  static auto sine_fn = build::SI1_SO<float, float>("Sine", [](const float a) { return sinf(a); });
  registry::register_function("sin(float)"_ustr, sine_fn);
}

}  // namespace blender::fn::multi_function
