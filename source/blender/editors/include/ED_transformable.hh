/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup animrig
 *
 * \brief Defines an abstraction around various structs to modify their transform properties via a
 * unified API.
 */

#pragma once

#include "BLI_array.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_span.hh"

#include "DNA_action_types.h"

#include "RNA_types.hh"

namespace blender {

struct bPoseChannel;
struct ID;
struct PointerRNA;
struct PropertyRNA;
class StringRef;
class StringRefNull;

namespace animrig {

/**
 * Used to limit the modification of properties to certain axes.
 */
enum AxisFlag : int8_t {
  AXIS_FLAG_NONE = 0,
  AXIS_FLAG_X = 1 << 0,
  AXIS_FLAG_Y = 1 << 1,
  AXIS_FLAG_Z = 1 << 2,
  /* There is currently no support for a W axis. This was already the case when porting this enum
     from the pose slide code. */
};

/**
 * Interpolated the values linearly based on `factor` and returns a new Array. Asserts that boths
 * spans are the same length. With the factor at `0` the values will match `a`.
 */
Array<float> property_interpolated(Span<float> a, Span<float> b, float factor);

/**
 * Describes a rotation in a specific mode and can be used to convert into other modes.
 */
struct Rotation {
  /* The array size differs depending on the rotation mode. */
  Array<float> values;
  eRotationModes mode;

  /**
   * Returns a copy of the rotation in the given mode.
   */
  Rotation converted_to_mode(eRotationModes mode) const;
};

/**
 * Returns a rotation representing "no rotation" for the given mode.
 */
Rotation identity_rotation(eRotationModes mode);
/**
 * Returns a new rotation, interpolated between `a` and `b` based on `factor`. If `factor` is 0
 * the result is `a`.
 * If the rotation mode on `a` and `b` does not match, then `b` is converted into the mode of `a`
 * first. The returned rotation is always in the rotation mode of `a`.
 * Quaternion rotations use spherical interpolation, all other modes use linear.
 */
Rotation rotation_interpolated(const Rotation &a, const Rotation &b, float factor);

/**
 * Provides a common interface to transform values for multiple structs.
 * In a way this is similar to RNA, however RNA has the issue that the properties don't have
 * consistent naming making it impossible to work with them in a generic way.
 */
class Transformable {
 public:
  enum class Type : int8_t {
    POSE_BONE,
    OBJECT,
  };

  enum class PropertyType : int8_t { LOCATION, ROTATION, SCALE };

 private:
  Type type_;
  ID *owner_id_;
  void *data_;

  /* This is the path from the owner ID to the struct that the Transformable represents. Has to be
   * created in the constructor. For structs that are an ID this is an empty string. */
  std::string rna_path_from_id_;

  /* We are assuming here that the ground truth of transforms is store in separate loc rot scale
   * and not in a matrix, thus skew is not supported. */
  MutableSpan<float> location_;
  /* Rotation can be expressed different modes, which are stored in separate arrays. We have to use
   * an Array of float* because the angle of axisangle is a separate float property. */
  Array<Array<float *>> rotations_;
  short *rotation_mode_;
  MutableSpan<float> scale_;

  /**
   * Returns the correct array based on the given mode. Asserts that the array is set for the
   * current transformable.
   */
  const Array<float *> *get_rotation_array_from_mode(eRotationModes mode) const;

 public:
  /* There has to be a constructor for every struct supported. */
  /* Constructor for pose bones. */
  Transformable(Object &owner_id, bPoseChannel &pchan);
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
   * Returns a string to the given property type.
   */
  std::string rna_path_to_property(PropertyType prop_type) const;
  std::string rna_path_to_rotation_mode(eRotationModes rotation_mode) const;

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
  void set_property(PropertyType prop_type, Span<float> values, AxisFlag axis_flag);
  /**
   * Do a linear blend of the property values towards the given `target`. It is asserted that the
   * given span size equals the property size. When setting the rotation property, it is the
   * responsibility of the caller to ensure that the values are in the correct mode. It is assumed
   * the `target` is in the same rotation mode as the transformable.
   *
   * \note This will not work correctly for quaternion rotations. Use `blend_rotation_to` instead.
   */
  void blend_property_to(PropertyType prop_type,
                         Span<float> target,
                         float factor,
                         AxisFlag axis_flag);
  /**
   * Overloaded function that blends all values of the given property type to the same float.
   */
  void blend_property_to(PropertyType prop_type, float target, float factor, AxisFlag axis_flag);

  /**
   * Returns a copy of the rotation in the mode the transformable is currently in.
   */
  Rotation get_rotation() const;
  Rotation get_rotation_for_mode(eRotationModes mode) const;
  /**
   * Sets the rotation for the mode the transformable is currently in. If that doesn't match with
   * the given rotation, the `value` is converted.
   */
  void set_rotation(const Rotation &value);
  /**
   * Returns the current rotation mode of the transformable.
   */
  eRotationModes get_rotation_mode() const;

  /**
   * Blends the rotation to the given `target`. If the rotation mode of the transformable and that
   * of the `target` does not match, the `target` is converted. This uses the correct interpolation
   * math depending on the rotation mode.
   */
  void blend_rotation_to(const Rotation &target, float factor, AxisFlag axis_flag);
};

}  // namespace animrig
}  // namespace blender
