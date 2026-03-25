/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_node_types.h"

#include "BLI_math_base_safe.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.hh"
#include "BLI_string_ref.hh"
#include "BLI_ustring.hh"

#include "FN_multi_function_builder.hh"

#include "NOD_multi_function.hh"

namespace blender::nodes {

void node_math_build_multi_function(NodeMultiFunctionBuilder &builder);

struct FloatMathOperationInfo {
  StringRefNull title_case_name;
  StringRefNull shader_name;
  UString multi_function_name;

  FloatMathOperationInfo() = delete;
  FloatMathOperationInfo(StringRefNull title_case_name,
                         StringRefNull shader_name,
                         UString multi_function_name = {})
      : title_case_name(title_case_name),
        shader_name(shader_name),
        multi_function_name(multi_function_name)
  {
  }
};

const FloatMathOperationInfo *get_float_math_operation_info(int operation);
const FloatMathOperationInfo *get_float3_math_operation_info(int operation);
const FloatMathOperationInfo *get_float_compare_operation_info(int operation);

/**
 * This calls the `callback` with two arguments:
 * 1. The math function that takes a float as input and outputs a new float.
 * 2. A #FloatMathOperationInfo struct reference.
 * Returns true when the callback has been called, otherwise false.
 *
 * The math function that is passed to the callback is actually a lambda function that is different
 * for every operation. Therefore, if the callback is templated on the math function, it will get
 * instantiated for every operation separately. This has two benefits:
 * - The compiler can optimize the callback for every operation separately.
 * - A static variable declared in the callback will be generated for every operation separately.
 *
 * If separate instantiations are not desired, the callback can also take a function pointer with
 * the following signature as input instead: float (*math_function)(float a).
 */

/**
 * This is similar to try_dispatch_float_math_fl_to_fl, just with a different callback signature.
 */
template<typename Callback>
inline bool try_dispatch_float_math_fl3_fl3_to_fl3(const NodeVectorMathOperation operation,
                                                   Callback &&callback)
{
  using namespace blender::math;

  const FloatMathOperationInfo *info = get_float3_math_operation_info(operation);
  if (info == nullptr) {
    return false;
  }

  static auto exec_preset_fast = mf::build::exec_presets::AllSpanOrSingle();
  static auto exec_preset_slow = mf::build::exec_presets::Materialized();

  /* This is just a utility function to keep the individual cases smaller. */
  auto dispatch = [&](auto exec_preset, auto math_function) -> bool {
    callback(exec_preset, math_function, *info);
    return true;
  };

  switch (operation) {
    case NODE_VECTOR_MATH_ADD:
      return dispatch(exec_preset_fast, [](float3 a, float3 b) { return a + b; });
    case NODE_VECTOR_MATH_SUBTRACT:
      return dispatch(exec_preset_fast, [](float3 a, float3 b) { return a - b; });
    case NODE_VECTOR_MATH_MULTIPLY:
      return dispatch(exec_preset_fast, [](float3 a, float3 b) { return a * b; });
    case NODE_VECTOR_MATH_DIVIDE:
      return dispatch(exec_preset_fast, [](float3 a, float3 b) { return safe_divide(a, b); });
    case NODE_VECTOR_MATH_CROSS_PRODUCT:
      return dispatch(exec_preset_fast,
                      [](float3 a, float3 b) { return cross_high_precision(a, b); });
    case NODE_VECTOR_MATH_PROJECT:
      return dispatch(exec_preset_fast, [](float3 a, float3 b) { return project(a, b); });
    case NODE_VECTOR_MATH_REFLECT:
      return dispatch(exec_preset_fast,
                      [](float3 a, float3 b) { return reflect(a, normalize(b)); });
    case NODE_VECTOR_MATH_SNAP:
      return dispatch(exec_preset_fast,
                      [](float3 a, float3 b) { return floor(safe_divide(a, b)) * b; });
    case NODE_VECTOR_MATH_MODULO:
      return dispatch(exec_preset_slow, [](float3 a, float3 b) { return mod(a, b); });
    case NODE_VECTOR_MATH_MINIMUM:
      return dispatch(exec_preset_fast, [](float3 a, float3 b) { return min(a, b); });
    case NODE_VECTOR_MATH_MAXIMUM:
      return dispatch(exec_preset_fast, [](float3 a, float3 b) { return max(a, b); });
    case NODE_VECTOR_MATH_POWER:
      return dispatch(exec_preset_slow, [](float3 a, float3 b) {
        return float3(safe_powf(a.x, b.x), safe_powf(a.y, b.y), safe_powf(a.z, b.z));
      });
    default:
      return false;
  }
  return false;
}

/**
 * This is similar to try_dispatch_float_math_fl_to_fl, just with a different callback signature.
 */
template<typename Callback>
inline bool try_dispatch_float_math_fl3_fl3_to_fl(const NodeVectorMathOperation operation,
                                                  Callback &&callback)
{
  using namespace blender::math;

  const FloatMathOperationInfo *info = get_float3_math_operation_info(operation);
  if (info == nullptr) {
    return false;
  }

  static auto exec_preset_fast = mf::build::exec_presets::AllSpanOrSingle();

  /* This is just a utility function to keep the individual cases smaller. */
  auto dispatch = [&](auto exec_preset, auto math_function) -> bool {
    callback(exec_preset, math_function, *info);
    return true;
  };

  switch (operation) {
    case NODE_VECTOR_MATH_DOT_PRODUCT:
      return dispatch(exec_preset_fast, [](float3 a, float3 b) { return dot(a, b); });
    case NODE_VECTOR_MATH_DISTANCE:
      return dispatch(exec_preset_fast, [](float3 a, float3 b) { return distance(a, b); });
    default:
      return false;
  }
  return false;
}

/**
 * This is similar to try_dispatch_float_math_fl_to_fl, just with a different callback signature.
 */
template<typename Callback>
inline bool try_dispatch_float_math_fl3_fl3_fl3_to_fl3(const NodeVectorMathOperation operation,
                                                       Callback &&callback)
{
  using namespace blender::math;

  const FloatMathOperationInfo *info = get_float3_math_operation_info(operation);
  if (info == nullptr) {
    return false;
  }

  static auto exec_preset_fast = mf::build::exec_presets::AllSpanOrSingle();
  static auto exec_preset_slow = mf::build::exec_presets::Materialized();

  /* This is just a utility function to keep the individual cases smaller. */
  auto dispatch = [&](auto exec_preset, auto math_function) -> bool {
    callback(exec_preset, math_function, *info);
    return true;
  };

  switch (operation) {
    case NODE_VECTOR_MATH_MULTIPLY_ADD:
      return dispatch(exec_preset_fast, [](float3 a, float3 b, float3 c) { return a * b + c; });
    case NODE_VECTOR_MATH_WRAP:
      return dispatch(exec_preset_slow, [](float3 a, float3 b, float3 c) {
        return float3(wrapf(a.x, b.x, c.x), wrapf(a.y, b.y, c.y), wrapf(a.z, b.z, c.z));
      });
    case NODE_VECTOR_MATH_FACEFORWARD:
      return dispatch(exec_preset_fast,
                      [](float3 a, float3 b, float3 c) { return faceforward(a, b, c); });
    default:
      return false;
  }
  return false;
}

/**
 * This is similar to try_dispatch_float_math_fl_to_fl, just with a different callback signature.
 */
template<typename Callback>
inline bool try_dispatch_float_math_fl3_fl3_fl_to_fl3(const NodeVectorMathOperation operation,
                                                      Callback &&callback)
{
  using namespace blender::math;

  const FloatMathOperationInfo *info = get_float3_math_operation_info(operation);
  if (info == nullptr) {
    return false;
  }

  static auto exec_preset_slow = mf::build::exec_presets::Materialized();

  /* This is just a utility function to keep the individual cases smaller. */
  auto dispatch = [&](auto exec_preset, auto math_function) -> bool {
    callback(exec_preset, math_function, *info);
    return true;
  };

  switch (operation) {
    case NODE_VECTOR_MATH_REFRACT:
      return dispatch(exec_preset_slow,
                      [](float3 a, float3 b, float c) { return refract(a, normalize(b), c); });
    default:
      return false;
  }
  return false;
}

/**
 * This is similar to try_dispatch_float_math_fl_to_fl, just with a different callback signature.
 */
template<typename Callback>
inline bool try_dispatch_float_math_fl3_to_fl(const NodeVectorMathOperation operation,
                                              Callback &&callback)
{
  using namespace blender::math;

  const FloatMathOperationInfo *info = get_float3_math_operation_info(operation);
  if (info == nullptr) {
    return false;
  }

  static auto exec_preset_fast = mf::build::exec_presets::AllSpanOrSingle();

  /* This is just a utility function to keep the individual cases smaller. */
  auto dispatch = [&](auto exec_preset, auto math_function) -> bool {
    callback(exec_preset, math_function, *info);
    return true;
  };

  switch (operation) {
    case NODE_VECTOR_MATH_LENGTH:
      return dispatch(exec_preset_fast, [](float3 in) { return length(in); });
    default:
      return false;
  }
  return false;
}

/**
 * This is similar to try_dispatch_float_math_fl_to_fl, just with a different callback signature.
 */
template<typename Callback>
inline bool try_dispatch_float_math_fl3_fl_to_fl3(const NodeVectorMathOperation operation,
                                                  Callback &&callback)
{
  const FloatMathOperationInfo *info = get_float3_math_operation_info(operation);
  if (info == nullptr) {
    return false;
  }

  static auto exec_preset_fast = mf::build::exec_presets::AllSpanOrSingle();

  /* This is just a utility function to keep the individual cases smaller. */
  auto dispatch = [&](auto exec_preset, auto math_function) -> bool {
    callback(exec_preset, math_function, *info);
    return true;
  };

  switch (operation) {
    case NODE_VECTOR_MATH_SCALE:
      return dispatch(exec_preset_fast, [](float3 a, float b) { return a * b; });
    default:
      return false;
  }
  return false;
}

/**
 * This is similar to try_dispatch_float_math_fl_to_fl, just with a different callback signature.
 */
template<typename Callback>
inline bool try_dispatch_float_math_fl3_to_fl3(const NodeVectorMathOperation operation,
                                               Callback &&callback)
{
  using namespace blender::math;

  const FloatMathOperationInfo *info = get_float3_math_operation_info(operation);
  if (info == nullptr) {
    return false;
  }

  static auto exec_preset_fast = mf::build::exec_presets::AllSpanOrSingle();
  static auto exec_preset_slow = mf::build::exec_presets::Materialized();

  /* This is just a utility function to keep the individual cases smaller. */
  auto dispatch = [&](auto exec_preset, auto math_function) -> bool {
    callback(exec_preset, math_function, *info);
    return true;
  };

  switch (operation) {
    case NODE_VECTOR_MATH_NORMALIZE:
      /* Should be safe. */
      return dispatch(exec_preset_fast, [](float3 in) { return normalize(in); });
    case NODE_VECTOR_MATH_ROUND:
      return dispatch(exec_preset_fast, [](float3 in) { return floor(in + 0.5f); });
    case NODE_VECTOR_MATH_FLOOR:
      return dispatch(exec_preset_fast, [](float3 in) { return floor(in); });
    case NODE_VECTOR_MATH_CEIL:
      return dispatch(exec_preset_fast, [](float3 in) { return ceil(in); });
    case NODE_VECTOR_MATH_FRACTION:
      return dispatch(exec_preset_fast, [](float3 in) { return fract(in); });
    case NODE_VECTOR_MATH_ABSOLUTE:
      return dispatch(exec_preset_fast, [](float3 in) { return abs(in); });
    case NODE_VECTOR_MATH_SIGN:
      return dispatch(exec_preset_fast, [](float3 in) { return sign(in); });
    case NODE_VECTOR_MATH_SINE:
      return dispatch(exec_preset_slow,
                      [](float3 in) { return float3(sinf(in.x), sinf(in.y), sinf(in.z)); });
    case NODE_VECTOR_MATH_COSINE:
      return dispatch(exec_preset_slow,
                      [](float3 in) { return float3(cosf(in.x), cosf(in.y), cosf(in.z)); });
    case NODE_VECTOR_MATH_TANGENT:
      return dispatch(exec_preset_slow,
                      [](float3 in) { return float3(tanf(in.x), tanf(in.y), tanf(in.z)); });
    default:
      return false;
  }
  return false;
}

}  // namespace blender::nodes
