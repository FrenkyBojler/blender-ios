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

enum AxisFlag : int8_t {
  AXIS_FLAG_NONE = 0,
  AXIS_FLAG_X = 1 << 0,
  AXIS_FLAG_Y = 1 << 1,
  AXIS_FLAG_Z = 1 << 2,
  AXIS_FLAG_W = 1 << 3,
};

/**
 * Interpolated the values linearly based on `factor` and returns a new Array. Asserts that boths
 * spans are the same length.
 */
Array<float> property_interpolated(Span<float> a, Span<float> b, float factor);

/* Describes a rotation in a specific mode. */
struct Rotation {
  /* The array size differs depending on the rotation mode. */
  Array<float> values;
  eRotationModes mode;

  /* Returns a copy of the rotation in the given mode. */
  Rotation converted_to_mode(eRotationModes mode) const;
  /* Returns a unit rotation for the given mode. */
  static Rotation unit_rotation(eRotationModes mode);
  static Rotation interpolated(const Rotation &a, const Rotation &b, float factor);
};

/**
 * Provides a common interface to transform values for multiple structs.
 * In a way this is similar to RNA, however RNA has the issue that the properties don't have
 * consistent naming making it not possible to work with them in a generic way.
 */
class Transformable {
 public:
  enum class Type : int8_t {
    POSE_BONE,
    OBJECT,
  };

  /* For generic access to property values. */
  enum class PropertyType : int8_t { LOCATION, ROTATION, SCALE };

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

  /**
   * Returns the correct array based on the given mode. Asserts that the array is set for the
   * current transformable.
   */
  const Array<float *> *get_rotation_array_from_mode(eRotationModes mode) const;

 public:
  Transformable(Object &obj, bPoseChannel &pchan);
  Transformable(Object &object);

  Type type() const
  {
    return type_;
  }

  ID *owner_id() const
  {
    return owner_id_;
  }

  void *data() const
  {
    return data_;
  }

  /* Returns the rna path from the ID to the struct represented by this transformable. If the
   * struct is an ID this is an empty string. */
  StringRefNull rna_path() const;

  /**
   * Returns a copy of the property values for the given property type.
   * While this will return the values of the current rotation mode for PropertyType::ROTATION, it
   * is best to use the explicit function for it so a `Rotation` struct is returned which has more
   * features for dealing with different rotation modes.
   */
  Array<float> get_property(PropertyType prop_type) const;
  /**
   * Generic way to set the given transform property. It is asserted that the value count matches
   * the current rotation mode. Use `set_rotation` to automatically convert to the correct mode.
   */
  void set_property(PropertyType prop_type, Span<float> values);
  /**
   * Do a linear blend of the property values towards the given `target`. It is asserted that the
   * given span size equals the property size.
   */
  void blend_property_to(PropertyType prop_type,
                         Span<float> target,
                         float factor,
                         AxisFlag axis_flag);

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
  eRotationModes get_rotation_mode() const;

  /**
   * Blend all location values to a single value.
   */
  void blend_location_to(float value, float factor, AxisFlag lock_flag);
  /* Blend the location to the values given in the span. The span size has to match the location
   * value count. */
  void blend_location_to(Span<float> values, float factor, AxisFlag lock_flag);

  void blend_scale_to(float target, float factor, AxisFlag axis_flag);

  void blend_rotation_to(const Rotation &target, float factor, AxisFlag axis_flag);
};

}  // namespace animrig
}  // namespace blender
