/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup animrig
 */
#include "BLI_math_rotation.h"
#include "BLI_string.h"

#include "DNA_object_types.h"

#include "ANIM_transformable.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

namespace blender::animrig {

static bool should_modify_axis(const int index, const AxisFlag axis_flag)
{
  if (axis_flag == AXIS_FLAG_NONE) {
    return true;
  }
  return axis_flag & (1 << index);
}

static Array<float> array_from_span(const Span<float> value)
{
  Array<float> copy(value.size());
  for (int i : value.index_range()) {
    copy[i] = value[i];
  }
  return copy;
}

static Array<float> array_from_span(const Span<float *> value)
{
  Array<float> copy(value.size());
  for (int i : value.index_range()) {
    copy[i] = *(value[i]);
  }
  return copy;
}

static void copy_span_into_mutable_span(const Span<float> value, MutableSpan<float> target)
{
  BLI_assert(target.size() == value.size());
  for (const int i : IndexRange(3)) {
    target[i] = value[i];
  }
}

static void blend_linear(MutableSpan<float> values,
                         const float target,
                         const float factor,
                         const AxisFlag axis_flag)
{
  for (int i : values.index_range()) {
    if (!should_modify_axis(i, axis_flag)) {
      continue;
    }
    values[i] += factor * (target - values[i]);
  }
}

static void blend_linear(MutableSpan<float> values,
                         const Span<float> target,
                         const float factor,
                         const AxisFlag axis_flag)
{
  for (int i : values.index_range()) {
    if (!should_modify_axis(i, axis_flag)) {
      continue;
    }
    values[i] += factor * (target[i] - values[i]);
  }
}

/* Using a namespace instead of enum class because these should still be used as indices into an
 * array and an enum class would require casting for that. */
namespace RotationModeIndex {
enum Indices : uint8_t {
  QUATERNION,
  AXIS_ANGLE,
  EULER,
  /* Not a rotation mode, always keep last. */
  MAX_ENUM,
};
}

Rotation Rotation::converted_to_mode(eRotationModes mode) const
{
  if (mode == this->mode) {
    return *this;
  }

  float4 quat;
  switch (this->mode) {
    case ROT_MODE_QUAT:
      copy_qt_qt(quat, this->values.data());
      break;

    case ROT_MODE_AXISANGLE:
      axis_angle_to_quat(quat, this->values.data(), this->values[3]);
      break;

    default:
      eulO_to_quat(quat, this->values.data(), this->mode);
      break;
  }

  Rotation converted;
  converted.mode = mode;
  switch (mode) {
    case ROT_MODE_QUAT:
      converted.values.reinitialize(4);
      copy_qt_qt(converted.values.data(), quat);
      break;

    case ROT_MODE_AXISANGLE:
      converted.values.reinitialize(4);
      quat_to_axis_angle(converted.values.data(), &converted.values[3], quat);
      break;

    default:
      converted.values.reinitialize(3);
      quat_to_eulO(converted.values.data(), mode, quat);
      break;
  }
  return converted;
}

Rotation Rotation::unit_rotation(const eRotationModes mode)
{
  switch (mode) {
    case ROT_MODE_QUAT:
      return {{1, 0, 0, 0}, mode};
    case ROT_MODE_AXISANGLE:
      return {{0, 1, 0, 0}, mode};
    default:
      return {{0, 0, 0}, mode};
  }
}

Rotation Rotation::interpolated(const Rotation &a, const Rotation &b, const float factor)
{
  /* TODO: lift this limiation. */
  BLI_assert(a.mode == b.mode);

  Rotation interpolated;
  interpolated.mode = a.mode;
  interpolated.values.reinitialize(a.values.size());
  switch (a.mode) {
    case ROT_MODE_QUAT:
      interp_qt_qtqt(interpolated.values.data(), a.values.data(), b.values.data(), factor);
      break;

    default:
      /* Should axis angle use a different interpolation mode? */
      for (int i : interpolated.values.index_range()) {
        interpolated.values[i] = interpf(a.values[i], b.values[i], factor);
      }
      break;
  }

  return interpolated;
}

StringRefNull Transformable::rna_path() const
{
  return rna_path_from_id_;
}

Array<float> Transformable::get_property(const PropertyType prop_type) const
{
  switch (prop_type) {
    case PropertyType::LOCATION:
      return array_from_span(location_);

    case PropertyType::ROTATION: {
      const Array<float *> *rotation_array = get_rotation_array_from_mode(
          eRotationModes(*rotation_mode_));
      return array_from_span(*rotation_array);
    }
    case PropertyType::SCALE:
      return array_from_span(scale_);
  }

  BLI_assert_unreachable();
  return {};
}

void Transformable::set_property(const PropertyType prop_type, const Span<float> values)
{
  switch (prop_type) {
    case PropertyType::LOCATION:
      copy_span_into_mutable_span(values, location_);
      break;

    case PropertyType::ROTATION: {
      const Array<float *> *rotation_array = get_rotation_array_from_mode(
          eRotationModes(*rotation_mode_));
      for (int i : rotation_array->index_range()) {
        if (i >= values.size()) {
          /* Trying to set a rotation with different mode. Use `set_rotation` instead. */
          BLI_assert_unreachable();
          return;
        }
        *(*rotation_array)[i] = values[i];
      }
      break;
    }
    case PropertyType::SCALE:
      copy_span_into_mutable_span(values, scale_);
      break;
  }
}

void Transformable::blend_property_to(const PropertyType prop_type,
                                      const Span<float> values,
                                      const float factor,
                                      const AxisFlag axis_flag)
{
  switch (prop_type) {
    case PropertyType::LOCATION:
      blend_linear(location_, values, factor, axis_flag);
      break;

    case PropertyType::ROTATION: {
      const Array<float *> *rotation_array = get_rotation_array_from_mode(
          eRotationModes(*rotation_mode_));
      if (rotation_array->size() != values.size()) {
        /* This doesn't catch all invalid cases. Differing euler rotation order or quaternion/axis
         * angle will still have the same array size but blending will create bogus data. */
        BLI_assert_msg(false, "Cannot do blending with differing rotation modes");
        return;
      }
      Rotation rotation;
      /* Note: We assume the rotation mode here which may not be correct. It is the responsibility
       * of the caller to ensure this is right or use `blend_rotation_to`. */
      rotation.mode = eRotationModes(*rotation_mode_);
      rotation.values = array_from_span(values);
      blend_rotation_to(rotation, factor, axis_flag);
      break;
    }
    case PropertyType::SCALE:
      blend_linear(scale_, values, factor, axis_flag);
      break;
  }
}

static std::string get_pose_bone_rna_path(const bPoseChannel &pose_bone)
{
  char name_esc[sizeof(pose_bone.name) * 2];
  BLI_str_escape(name_esc, pose_bone.name, sizeof(name_esc));
  return fmt::format("pose.bones[\"{}\"]", name_esc);
}

static void build_rotations_array(
    Array<Array<float *>> &rotations, float *euler, float *quat, float *axis, float *angle)
{
  rotations.reinitialize(RotationModeIndex::MAX_ENUM);
  rotations[RotationModeIndex::EULER] = Array<float *>(3);
  for (int i : IndexRange(3)) {
    rotations[RotationModeIndex::EULER][i] = &euler[i];
  }

  rotations[RotationModeIndex::QUATERNION] = Array<float *>(4);
  for (int i : IndexRange(4)) {
    rotations[RotationModeIndex::QUATERNION][i] = &quat[i];
  }

  rotations[RotationModeIndex::AXIS_ANGLE] = Array<float *>(4);
  for (int i : IndexRange(3)) {
    rotations[RotationModeIndex::AXIS_ANGLE][i] = &axis[i];
  }
  rotations[RotationModeIndex::AXIS_ANGLE][3] = angle;
}

Transformable::Transformable(Object &obj, bPoseChannel &pchan)
    : type_(Transformable::Type::POSE_BONE),
      owner_id_(&obj.id),
      data_(&pchan),
      location_({pchan.loc, 3}),
      rotation_mode_(&pchan.rotmode),
      scale_({pchan.scale, 3})
{
  build_rotations_array(rotations_, pchan.eul, pchan.quat, pchan.rotAxis, &pchan.rotAngle);
  rna_path_from_id_ = get_pose_bone_rna_path(pchan);
}

Transformable::Transformable(Object &obj)
    : type_(Transformable::Type::OBJECT),
      owner_id_(&obj.id),
      data_(&obj),
      location_({obj.loc, 3}),
      scale_({obj.scale, 3})
{
  build_rotations_array(rotations_, obj.rot, obj.quat, obj.rotAxis, &obj.rotAngle);
  rna_path_from_id_ = "";
}

Array<float> Transformable::get_location() const
{
  return array_from_span(location_);
}

void Transformable::set_location(const Span<float> value)
{
  copy_span_into_mutable_span(value, location_);
}

void Transformable::set_location(const float3 value)
{
  BLI_assert(location_.size() == 3);
  copy_span_into_mutable_span(Span<float>(value, 3), location_);
}

Array<float> Transformable::get_scale() const
{
  return array_from_span(scale_);
}

void Transformable::set_scale(const Span<float> value)
{
  copy_span_into_mutable_span(value, scale_);
}

void Transformable::set_scale(const float3 value)
{
  BLI_assert(scale_.size() == 3);
  copy_span_into_mutable_span(Span<float>(value, 3), scale_);
}

const Array<float *> *Transformable::get_rotation_array_from_mode(const eRotationModes mode) const
{
  const Array<float *> *rotations_array = nullptr;
  switch (mode) {
    case ROT_MODE_QUAT:
      rotations_array = &rotations_[RotationModeIndex::QUATERNION];
      break;
    case ROT_MODE_AXISANGLE:
      rotations_array = &rotations_[RotationModeIndex::AXIS_ANGLE];
      break;
    default:
      rotations_array = &rotations_[RotationModeIndex::EULER];
      break;
  }
  BLI_assert(!rotations_array->is_empty());
  return rotations_array;
}

Rotation Transformable::get_rotation() const
{
  Rotation rotation;
  rotation.mode = eRotationModes(*rotation_mode_);
  const Array<float *> *rotations_array = get_rotation_array_from_mode(rotation.mode);
  BLI_assert(rotations_array != nullptr);
  rotation.values = array_from_span(*rotations_array);
  return rotation;
}

void Transformable::set_rotation(const Rotation &rotation)
{
  const eRotationModes current_mode = eRotationModes(*rotation_mode_);
  const Array<float *> *rotations_array = get_rotation_array_from_mode(current_mode);
  BLI_assert(rotations_array != nullptr);
  if (rotation.mode == current_mode) {
    /* Easy case, can just copy the values. */
    for (int i : rotations_array->index_range()) {
      *(*rotations_array)[i] = rotation.values[i];
    }
    return;
  }

  Rotation rot_in_correct_mode = rotation.converted_to_mode(current_mode);
  for (int i : rotations_array->index_range()) {
    *(*rotations_array)[i] = rot_in_correct_mode.values[i];
  }
}

eRotationModes Transformable::get_rotation_mode() const
{
  return eRotationModes(*rotation_mode_);
}

void Transformable::blend_location_to(const float target,
                                      const float factor,
                                      const AxisFlag axis_flag)
{
  blend_linear(location_, target, factor, axis_flag);
}

void Transformable::blend_location_to(const Span<float> target,
                                      const float factor,
                                      const AxisFlag axis_flag)
{
  blend_linear(location_, target, factor, axis_flag);
}

void Transformable::blend_scale_to(const float target,
                                   const float factor,
                                   const AxisFlag axis_flag)
{
  blend_linear(scale_, target, factor, axis_flag);
}

void Transformable::blend_rotation_to(const Rotation &target,
                                      const float factor,
                                      const AxisFlag axis_flag)
{
  const eRotationModes current_mode = eRotationModes(*rotation_mode_);
  Rotation rot;
  if (target.mode == current_mode) {
    rot = target;
  }
  else {
    rot = target.converted_to_mode(current_mode);
  }
  const Array<float *> *rotations_array = get_rotation_array_from_mode(current_mode);
  BLI_assert(rotations_array != nullptr);

  Array<float> result;
  switch (current_mode) {
    case ROT_MODE_QUAT: {
      float4 current_quat;
      for (int i : IndexRange(4)) {
        current_quat[i] = *((*rotations_array)[i]);
      }
      normalize_qt(current_quat);
      normalize_qt(rot.values.data());
      result.reinitialize(4);
      /* We are not using the axis flag here. Not sure how that would work with quaternions. */
      interp_qt_qtqt(result.data(), current_quat, rot.values.data(), factor);
      break;
    }
    case ROT_MODE_AXISANGLE: {
      result.reinitialize(4);
      for (int i : IndexRange(4)) {
        result[i] = *((*rotations_array)[i]);
      }
      /* Should this use spherical blending? */
      blend_linear(result, rot.values, factor, axis_flag);
      break;
    }
    default: {
      result.reinitialize(3);
      for (int i : IndexRange(3)) {
        result[i] = *((*rotations_array)[i]);
      }
      blend_linear(result, rot.values, factor, axis_flag);
      break;
    }
  }

  BLI_assert(result.size() == rotations_array->size());
  for (int i : result.index_range()) {
    *(*rotations_array)[i] = result[i];
  }
}

}  // namespace blender::animrig
