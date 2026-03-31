/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup animrig
 *
 * \brief Defines an abstraction around various structs to modify their animation via a unified
 * API.
 */

#include "BLI_math_matrix_types.hh"

#include "DNA_action_types.h"

#include "RNA_types.hh"

namespace blender {

struct PointerRNA;
struct PropertyRNA;
struct bPoseChannel;

namespace animrig {

enum class TransformableProperty {
  MATRIX_LOCAL,
  MATRIX_WORLD,
  LOCATION,
  ROTATION,
  SCALE,
  CUSTOM_PROPERTIES,
  BBONE,
};

/**
 * Provides a thin interface over an RNA struct with functions to get and set transform properties.
 */
class Transformable {

 public:
  PointerRNA ptr_;
  Transformable(PointerRNA &ptr) : ptr_(ptr){};
  /**
   * Returns a vector of PropertyRNA pointers to the given property types. The vector is guaranteed
   * to contain no nullptrs. Any props unknown to the underlying data are ignored.
   */
  Vector<PropertyRNA *> get_transformable_properties(Span<TransformableProperty> props);

  virtual float4x4 get_world_space() = 0;
  virtual PropertyRNA &get_local_space() = 0;

};

class TransformablePoseBone : Transformable {
  virtual PropertyRNA &get_local_space() override;
};

class TransformableObject : Transformable {
  virtual PropertyRNA &get_local_space() override;
};

}  // namespace animrig
}  // namespace blender
