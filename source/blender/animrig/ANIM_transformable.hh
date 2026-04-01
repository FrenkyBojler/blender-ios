/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup animrig
 *
 * \brief Defines an abstraction around various structs to modify their animation via a unified
 * API.
 */

#pragma once

#include "BLI_array.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_span.hh"

#include "DNA_action_types.h"

#include "RNA_types.hh"

namespace blender {

struct PointerRNA;
struct PropertyRNA;
struct bPoseChannel;
struct ID;

class StringRef;
class StringRefNull;

namespace animrig {

namespace ChannelFlag {
enum Flags : int8_t {
  NONE = 0,
  X = 1 << 0,
  Y = 1 << 1,
  Z = 1 << 2,
  W = 1 << 3,
};
}

/* Describes a rotation in a specific mode. */
struct Rotation {
  Array<float> values;
  eRotationModes mode;
};

class Transformable {
 public:
  enum class Type : int8_t {
    POSE_BONE,
    OBJECT,
  };

 private:
  Type type_;
  ID *owner_id_;
  void *data_;

  std::string rna_path_from_id_;

  /* We are assuming here that the ground truth of transforms is store in separate loc rot scale
   * and not in a matrix, thus skew is not supported. */
  MutableSpan<float> location_;
  /* Rotation can be expressed different modes, which are stored in separate arrays. We have to use
   * float* because the angle of axisangle is a separate float property. */
  Array<Array<float *>> rotations_;
  short *rotation_mode_;
  MutableSpan<float> scale_;

 public:
  Transformable(Object &obj, bPoseChannel &pchan);
  Transformable(Object &object);

  Type type()
  {
    return type_;
  }

  ID *owner_id()
  {
    return owner_id_;
  }

  void *data()
  {
    return data_;
  }

  StringRefNull rna_path();

  /* Returns a copy of the current location. */
  Array<float> get_location() const;
  void set_location(Span<float> value);
  void set_location(float3 value);

  Array<float> get_scale() const;
  void set_scale(Span<float> value);
  void set_scale(float3 value);

  /**
   * Returns a copy of the rotation in the mode the transformable is currently in.
   */
  Rotation get_rotation() const;
  /**
   * Sets the rotation for the mode the transformable is currently in. If that doesn't match with
   * the given rotation, a conversion is performed.
   */
  void set_rotation(const Rotation &value);

  /**
   * Blend all location values to a single value.
   */
  void blend_location_to(float value, float factor, ChannelFlag::Flags lock_flag);
  /* Blend the location to the values given in the span. The span size has to match the location
   * value count. */
  void blend_location_to(Span<float> values, float factor, ChannelFlag::Flags lock_flag);
};

}  // namespace animrig
}  // namespace blender
