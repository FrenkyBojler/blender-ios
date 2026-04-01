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

StringRefNull Transformable::rna_path()
{
  return rna_path_from_id_;
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

static Array<float> copy_span_to_array(const Span<float> value)
{
  Array<float> copy(value.size());
  for (int i : value.index_range()) {
    copy[i] = value[i];
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

Array<float> Transformable::get_location() const
{
  return copy_span_to_array(location_);
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
  return copy_span_to_array(scale_);
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

  rotation.values.reinitialize(rotations_array->size());
  for (int i : rotations_array->index_range()) {
    rotation.values[i] = *((*rotations_array)[i]);
  }
  return rotation;
}

void Transformable::set_rotation(const Rotation &rotation)
{
  const eRotationModes current_mode = eRotationModes(*rotation_mode_);
  const Array<float *> *rotations_array = get_rotation_array_from_mode(current_mode);
  BLI_assert(rotations_array != nullptr);
  if (rotation.mode == *rotation_mode_) {
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

static bool should_modify_axis(const int index, const AxisFlag::Flags axis_flag)
{
  if (axis_flag == AxisFlag::NONE) {
    return true;
  }
  return axis_flag & (1 << index);
}

static void blend_linear(MutableSpan<float> values,
                         const float target,
                         const float factor,
                         const AxisFlag::Flags axis_flag)
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
                         const AxisFlag::Flags axis_flag)
{
  for (int i : values.index_range()) {
    if (!should_modify_axis(i, axis_flag)) {
      continue;
    }
    values[i] += factor * (target[i] - values[i]);
  }
}

void Transformable::blend_location_to(const float target,
                                      const float factor,
                                      const AxisFlag::Flags axis_flag)
{
  blend_linear(location_, target, factor, axis_flag);
}

void Transformable::blend_location_to(const Span<float> target,
                                      const float factor,
                                      const AxisFlag::Flags axis_flag)
{
  blend_linear(location_, target, factor, axis_flag);
}

}  // namespace blender::animrig
