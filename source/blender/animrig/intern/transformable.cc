/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup animrig
 */
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

Rotation Transformable::get_rotation() const
{
  const Array<float *> *rotations_array = nullptr;
  Rotation rotation;
  rotation.mode = eRotationModes(*rotation_mode_);
  switch (*rotation_mode_) {
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

  rotation.values.reinitialize(rotations_array->size());
  for (int i : rotations_array->index_range()) {
    rotation.values[i] = *((*rotations_array)[i]);
  }
  return rotation;
}

void Transformable::set_rotation(const Rotation &value) {}

static void blend_linear(MutableSpan<float> values,
                         const float target,
                         const float factor,
                         const ChannelFlag::Flags lock_flag)
{
  for (int i : values.index_range()) {
    values[i] += factor * (target - values[i]);
  }
}

static void blend_linear(MutableSpan<float> values,
                         const Span<float> target,
                         const float factor,
                         const ChannelFlag::Flags lock_flag)
{
  for (int i : values.index_range()) {
    values[i] += factor * (target[i] - values[i]);
  }
}

void Transformable::blend_location_to(const float target,
                                      const float factor,
                                      const ChannelFlag::Flags lock_flag)
{
  blend_linear(location_, target, factor, lock_flag);
}

void Transformable::blend_location_to(const Span<float> target,
                                      const float factor,
                                      const ChannelFlag::Flags lock_flag)
{
  blend_linear(location_, target, factor, lock_flag);
}

}  // namespace blender::animrig
