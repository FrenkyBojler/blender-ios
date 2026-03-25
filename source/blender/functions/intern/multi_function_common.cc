/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_base_safe.h"
#include "FN_init.hh"
#include "FN_multi_function_builder.hh"
#include "FN_multi_function_registry.hh"

namespace blender::fn::multi_function {

void register_common_functions()
{
  static constexpr auto exec_fast = build::exec_presets::AllSpanOrSingle();

  registry::add_new_cb([] {
    return build::SI1_SO<float, float>("exp(float)", [](const float a) { return expf(a); });
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>(
        "sqrt(float)", [](const float a) { return safe_sqrtf(a); }, exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>(
        "inverse_sqrt(float)", [](const float a) { return safe_inverse_sqrtf(a); }, exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>(
        "abs(float)", [](const float a) { return fabsf(a); }, exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>(
        "radians(float)", [](const float a) { return float(DEG2RAD(a)); }, exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>(
        "degrees(float)", [](const float a) { return float(RAD2DEG(a)); }, exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>(
        "sign(float)", [](const float a) { return compatible_signf(a); }, exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>(
        "round(float)", [](const float a) { return floorf(a + 0.5f); }, exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>(
        "floor(float)", [](const float a) { return floorf(a); }, exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>(
        "ceil(float)", [](const float a) { return ceilf(a); }, exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>(
        "frac(float)", [](const float a) { return a - floorf(a); }, exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>(
        "trunc(float)", [](const float a) { return a >= 0.0f ? floorf(a) : ceilf(a); }, exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>("sin(float)", [](const float a) { return sinf(a); });
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>("cos(float)", [](const float a) { return cosf(a); });
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>("tan(float)", [](const float a) { return tanf(a); });
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>("sinh(float)", [](const float a) { return sinhf(a); });
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>("cosh(float)", [](const float a) { return coshf(a); });
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>("tanh(float)", [](const float a) { return tanhf(a); });
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>("asin(float)", [](const float a) { return safe_asinf(a); });
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>("acos(float)", [](const float a) { return safe_acosf(a); });
  });
  registry::add_new_cb([] {
    return build::SI1_SO<float, float>("atan(float)", [](const float a) { return atanf(a); });
  });

  registry::add_new_cb([] {
    return build::SI2_SO<float, float, float>(
        "float + float", [](const float a, const float b) { return a + b; }, exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI2_SO<float, float, float>(
        "float - float", [](const float a, const float b) { return a - b; }, exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI2_SO<float, float, float>(
        "float * float", [](const float a, const float b) { return a * b; }, exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI2_SO<float, float, float>(
        "float / float",
        [](const float a, const float b) { return safe_divide(a, b); },
        exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI2_SO<float, float, float>(
        "float ^ float", [](const float a, const float b) { return safe_powf(a, b); }, exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI2_SO<float, float, float>(
        "log(float, float)",
        [](const float a, const float b) { return safe_logf(a, b); },
        exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI2_SO<float, float, float>(
        "min(float, float)",
        [](const float a, const float b) { return std::min(a, b); },
        exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI2_SO<float, float, float>(
        "max(float, float)",
        [](const float a, const float b) { return std::max(a, b); },
        exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI2_SO<float, float, float>(
        "float < float", [](const float a, const float b) { return float(a < b); }, exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI2_SO<float, float, float>(
        "float > float", [](const float a, const float b) { return float(a > b); }, exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI2_SO<float, float, float>(
        "float % float", [](const float a, const float b) { return safe_modf(a, b); }, exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI2_SO<float, float, float>(
        "floor_mod(float, float)",
        [](const float a, const float b) { return safe_floored_modf(a, b); },
        exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI2_SO<float, float, float>(
        "snap(float, float)",
        [](const float a, const float b) { return floorf(safe_divide(a, b)) * b; },
        exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI2_SO<float, float, float>(
        "atan2(float, float)",
        [](const float a, const float b) { return atan2f(a, b); },
        exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI2_SO<float, float, float>(
        "pingpong(float, float)",
        [](const float a, const float b) { return pingpongf(a, b); },
        exec_fast);
  });

  registry::add_new_cb([] {
    return build::SI3_SO<float, float, float, float>(
        "float * float + float",
        [](const float a, const float b, const float c) { return a * b + c; },
        exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI3_SO<float, float, float, float>(
        "compare(float, float, float)",
        [](const float a, const float b, const float c) {
          return ((a == b) || (fabsf(a - b) <= fmaxf(c, FLT_EPSILON))) ? 1.0f : 0.0f;
        },
        exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI3_SO<float, float, float, float>(
        "smooth_min(float, float, float)",
        [](const float a, const float b, const float c) { return smoothminf(a, b, c); },
        exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI3_SO<float, float, float, float>(
        "smooth_max(float, float, float)",
        [](const float a, const float b, const float c) { return -smoothminf(-a, -b, c); },
        exec_fast);
  });
  registry::add_new_cb([] {
    return build::SI3_SO<float, float, float, float>(
        "wrap(float, float, float)",
        [](const float a, const float b, const float c) { return wrapf(a, b, c); },
        exec_fast);
  });
}

}  // namespace blender::fn::multi_function
