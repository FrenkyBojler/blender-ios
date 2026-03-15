/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * A #ValueElem is an abstract element or part of a value. It does not store the actual value of
 * the type but which parts of it are affected. For example, #VectorElem does not store the actual
 * vector values but just a boolean for each component.
 *
 * Some nodes implement special #node_eval_elem and #node_eval_inverse_elem methods which allow
 * analyzing the potential impact of changing part of a value in one place of a node tree.
 *
 * The types are generally quite small and trivially copyable and destructible.
 * They just contain some booleans.
 */

#include <optional>
#include <type_traits>
#include <variant>

#include "BLI_hash.hh"
#include "BLI_math_euler.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_quaternion_types.hh"
#include "BLI_math_vector_types.hh"

#include "DNA_node_types.h"

namespace blender::nodes::value_elem {

/**
 * Common base type value primitive types that can't be subdivided further.
 */
struct PrimitiveValueElem {
  bool affected = false;

  operator bool() const
  {
    return this->affected;
  }

  friend bool operator==(const PrimitiveValueElem &a, const PrimitiveValueElem &b) = default;

  void merge(const PrimitiveValueElem &other)
  {
    this->affected |= other.affected;
  }

  void intersect(const PrimitiveValueElem &other)
  {
    this->affected &= other.affected;
  }

  uint64_t hash() const
  {
    return get_default_hash(this->affected);
  }
};

struct BoolElem : public PrimitiveValueElem {
  static BoolElem all()
  {
    return {true};
  }
};

struct FloatElem : public PrimitiveValueElem {
  static FloatElem all()
  {
    return {true};
  }
};

struct IntElem : public PrimitiveValueElem {
  static IntElem all()
  {
    return {true};
  }
};

struct VectorElem {
  /** Members indicate which components of the vector are affected. */
  FloatElem x;
  FloatElem y;
  FloatElem z;

  operator bool() const
  {
    return this->x || this->y || this->z;
  }

  friend bool operator==(const VectorElem &a, const VectorElem &b) = default;

  uint64_t hash() const
  {
    return get_default_hash(this->x, this->y, this->z);
  }

  void merge(const VectorElem &other)
  {
    this->x.merge(other.x);
    this->y.merge(other.y);
    this->z.merge(other.z);
  }

  void intersect(const VectorElem &other)
  {
    this->x.intersect(other.x);
    this->y.intersect(other.y);
    this->z.intersect(other.z);
  }

  static VectorElem all()
  {
    return {{true}, {true}, {true}};
  }
};

struct RotationElem {
  /**
   * The euler and axis-angle components have overlap. All components that can be affected need to
   * be tagged. For example if a node affects the euler angles, it indirectly also affects the
   * axis-angle.
   */
  VectorElem euler;
  VectorElem axis;
  FloatElem angle;

  operator bool() const
  {
    return this->euler || this->axis || this->angle;
  }

  friend bool operator==(const RotationElem &a, const RotationElem &b) = default;

  uint64_t hash() const
  {
    return get_default_hash(this->euler, this->axis, this->angle);
  }

  void merge(const RotationElem &other)
  {
    this->euler.merge(other.euler);
    this->axis.merge(other.axis);
    this->angle.merge(other.angle);
  }

  void intersect(const RotationElem &other)
  {
    this->euler.intersect(other.euler);
    this->axis.intersect(other.axis);
    this->angle.intersect(other.angle);
  }

  static RotationElem all()
  {
    return {VectorElem::all(), VectorElem::all(), {true}};
  }
};

struct MatrixElem {
  VectorElem translation;
  RotationElem rotation;
  VectorElem scale;
  /** For 4x4 matrices this describes whether any entry of the last row is affected. */
  FloatElem any_non_transform;

  operator bool() const
  {
    return this->translation || this->rotation || this->scale || this->any_non_transform;
  }

  friend bool operator==(const MatrixElem &a, const MatrixElem &b) = default;

  uint64_t hash() const
  {
    return get_default_hash(
        this->translation, this->rotation, this->scale, this->any_non_transform);
  }

  void merge(const MatrixElem &other)
  {
    this->translation.merge(other.translation);
    this->rotation.merge(other.rotation);
    this->scale.merge(other.scale);
    this->any_non_transform.merge(other.any_non_transform);
  }

  void intersect(const MatrixElem &other)
  {
    this->translation.intersect(other.translation);
    this->rotation.intersect(other.rotation);
    this->scale.intersect(other.scale);
    this->any_non_transform.intersect(other.any_non_transform);
  }

  static MatrixElem all()
  {
    return {VectorElem::all(), RotationElem::all(), VectorElem::all(), FloatElem::all()};
  }
};

/**
 * A generic type that can hold the value element for any of the above types and has the same
 * interface. This should be used when the data type is not known at compile time.
 */
struct ElemVariant {
  std::variant<BoolElem, FloatElem, IntElem, VectorElem, RotationElem, MatrixElem> elem;

  operator bool() const
  {
    return std::visit([](const auto &value) { return bool(value); }, this->elem);
  }

  uint64_t hash() const
  {
    return std::visit([](auto &value) { return value.hash(); }, this->elem);
  }

  void merge(const ElemVariant &other)
  {
    BLI_assert(this->elem.index() == other.elem.index());
    std::visit(
        [&](auto &value) {
          using T = std::decay_t<decltype(value)>;
          value.merge(std::get<T>(other.elem));
        },
        this->elem);
  }

  void intersect(const ElemVariant &other)
  {
    BLI_assert(this->elem.index() == other.elem.index());
    std::visit(
        [&](auto &value) {
          using T = std::decay_t<decltype(value)>;
          value.intersect(std::get<T>(other.elem));
        },
        this->elem);
  }

  void set_all()
  {
    std::visit(
        [](auto &value) {
          using T = std::decay_t<decltype(value)>;
          value = T::all();
        },
        this->elem);
  }

  void clear_all()
  {
    std::visit(
        [](auto &value) {
          using T = std::decay_t<decltype(value)>;
          value = T();
        },
        this->elem);
  }

  friend bool operator==(const ElemVariant &a, const ElemVariant &b) = default;
};

template<typename T> inline constexpr bool always_false_v = false;

template<typename T> static ElemVariant compare(const T &value_old, const T &value_new)
{
  if constexpr (std::is_same_v<T, bool>) {
    return {BoolElem{value_old != value_new}};
  }
  else if constexpr (std::is_same_v<T, float>) {
    return {FloatElem{fabsf(value_old - value_new) > 1e-6f}};
  }
  else if constexpr (std::is_integral_v<T>) {
    return {IntElem{value_old != value_new}};
  }
  else if constexpr (std::is_same_v<T, float3>) {
    VectorElem elem;
    elem.x.affected = compare(value_old.x, value_new.x);
    elem.y.affected = compare(value_old.y, value_new.y);
    elem.z.affected = compare(value_old.z, value_new.z);
    return {elem};
  }
  else if constexpr (std::is_same_v<T, math::Quaternion>) {
    const float3 euler_old = float3(math::to_euler(value_old).xyz());
    const math::Quaternion value_new_wrapped = value_new.wrapped_around(value_old);
    const float3 euler_new = float3(math::to_euler(value_new_wrapped).xyz());

    RotationElem elem;
    elem.euler = std::get<VectorElem>(compare(euler_old, euler_new).elem);
    if (elem.euler) {
      elem.axis = VectorElem::all();
      elem.angle = FloatElem::all();
    }
    return {elem};
  }
  else if constexpr (std::is_same_v<T, float4x4>) {
    float loc_old[3], quat_old[4], scale_old[3];
    float loc_new[3], quat_new[4], scale_new[3];
    mat4_decompose(loc_old, quat_old, scale_old, value_old.ptr());
    mat4_decompose(loc_new, quat_new, scale_new, value_new.ptr());

    MatrixElem elem;
    elem.translation = std::get<VectorElem>(compare(float3(loc_old), float3(loc_new)).elem);
    elem.rotation = std::get<RotationElem>(
        compare(math::Quaternion(quat_old[0], quat_old[1], quat_old[2], quat_old[3]),
                math::Quaternion(quat_new[0], quat_new[1], quat_new[2], quat_new[3]))
            .elem);
    elem.scale = std::get<VectorElem>(compare(float3(scale_old), float3(scale_new)).elem);

    bool non_transform_affected = false;
    for (const int i : {0, 1, 2, 3}) {
      non_transform_affected |= value_old[i][3] != value_new[i][3];
    }
    elem.any_non_transform = FloatElem{non_transform_affected};
    return {elem};
  }
  else {
    static_assert(always_false_v<T>, "Unsupported value type for value_elem::compare");
  }
}

/** Utility struct to pair a socket with a value element. */
struct SocketElem {
  const bNodeSocket *socket = nullptr;
  ElemVariant elem;

  uint64_t hash() const
  {
    return get_default_hash(this->socket, this->elem);
  }

  friend bool operator==(const SocketElem &a, const SocketElem &b) = default;
};

/** Utility struct to pair a group input index with a value element. */
struct GroupInputElem {
  int group_input_index = 0;
  ElemVariant elem;

  uint64_t hash() const
  {
    return get_default_hash(this->group_input_index, this->elem);
  }

  friend bool operator==(const GroupInputElem &a, const GroupInputElem &b) = default;
};

/** Utility struct to pair a value node with a value element. */
struct ValueNodeElem {
  const bNode *node = nullptr;
  ElemVariant elem;

  uint64_t hash() const
  {
    return get_default_hash(this->node, this->elem);
  }

  friend bool operator==(const ValueNodeElem &a, const ValueNodeElem &b) = default;
};

/**
 * Get the default value element for the given socket type if it exists.
 */
std::optional<ElemVariant> get_elem_variant_for_socket_type(eNodeSocketDatatype type);

/** Converts the type of a value element if possible. */
std::optional<ElemVariant> convert_socket_elem(const bNodeSocket &old_socket,
                                               const bNodeSocket &new_socket,
                                               const ElemVariant &old_elem);

}  // namespace blender::nodes::value_elem
