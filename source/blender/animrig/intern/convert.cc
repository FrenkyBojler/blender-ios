#include "DNA_action_types.h"
#include "DNA_anim_types.h"
#include "DNA_object_types.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_set.hh"

#include "BKE_fcurve.hh"

#include "ANIM_action.hh"
#include "ANIM_convert.hh"
#include "ANIM_fcurve.hh"
#include "ANIM_rna.hh"

namespace blender::animrig {

static int compare_int(const void *a, const void *b)
{
  return *(static_cast<const int *>(a)) - *(static_cast<const int *>(b));
}
/*
 * Builds a sorted array of unique frames where at least one of the given FCurves has a key. This
 * uses int instead of float to avoid precision issues. The maximum subframe resolution is dicated
 * by BEZT_BINARYSEARCH_THRESH so this is used to convert to a unique integer.
 */
static Vector<int64_t> build_keyframe_ids(const Span<const FCurve *> fcurves)
{
  Vector<int64_t> keyframe_ids;
  Set<int64_t> existing_ids;
  for (const FCurve *fcurve : fcurves) {
    if (!fcurve || !fcurve->bezt) {
      continue;
    }
    for (int i = 0; i < fcurve->totvert; i++) {
      const int64_t value = int64_t(fcurve->bezt[i].vec[1][0] / BEZT_BINARYSEARCH_THRESH);
      if (existing_ids.add(value)) {
        keyframe_ids.append(value);
      }
    }
  }
  /* Keys have to be sorted to produce euler filtered results. */
  qsort(keyframe_ids.data(), keyframe_ids.size(), sizeof(int64_t), compare_int);
  return keyframe_ids;
}

static void rotation_values_to_quat(const float4 &rotation_values,
                                    const eRotationModes mode,
                                    float4 &r_quat)
{
  switch (mode) {
    case ROT_MODE_QUAT:
      copy_qt_qt(r_quat, rotation_values);
      break;

    case ROT_MODE_AXISANGLE:
      axis_angle_to_quat(r_quat, &rotation_values[1], rotation_values[0]);
      break;

    default:
      eulO_to_quat(r_quat, rotation_values, mode);
      break;
  }
}

static void quat_to_rotation_values(const float4 &quat,
                                    const eRotationModes mode,
                                    const float4 &reference_rotation,
                                    float4 &r_rotation_values)
{
  switch (mode) {
    case ROT_MODE_QUAT:
      copy_qt_qt(r_rotation_values, quat);
      break;

    case ROT_MODE_AXISANGLE:
      quat_to_axis_angle(&r_rotation_values[1], r_rotation_values, quat);
      break;

    default:
      quat_to_compatible_eulO(r_rotation_values, reference_rotation, mode, quat);
      break;
  }
}

static void get_rotation_values(const bPoseChannel &pose_bone, float4 &rotation_values)
{
  switch (pose_bone.rotmode) {
    case ROT_MODE_QUAT:
      copy_v4_v4(rotation_values, pose_bone.quat);
      break;

    case ROT_MODE_AXISANGLE:
      copy_v3_v3(&rotation_values[1], pose_bone.rotAxis);
      rotation_values[0] = pose_bone.rotAngle;
      break;

    default:
      copy_v3_v3(rotation_values, pose_bone.eul);
      break;
  }
}

bool convert_pose_bone_rotation_keys(Main *bmain,
                                     ID &owner_id,
                                     bPoseChannel &pchan,
                                     const RNAPathFCurveMap &fcurves_by_rna_path,
                                     const eRotationModes to_mode)
{
  if (pchan.rotmode == to_mode) {
    return false;
  }
  PointerRNA ptr = RNA_pointer_create_discrete(&owner_id, RNA_PoseBone, &pchan);
  const std::optional<std::string> pchan_path = RNA_path_from_ID_to_struct(&ptr);
  if (!pchan_path) {
    return false;
  }
  const eRotationModes current_mode = eRotationModes(pchan.rotmode);
  const StringRef rotation_mode = get_rotation_mode_path(current_mode);
  /* This is the current rotation mode path. */
  std::string current_rotation_path = pchan_path.value() + "." + rotation_mode;
  const ChannelbagFCurveMap *channelbag_map = fcurves_by_rna_path.lookup_ptr(
      current_rotation_path);
  if (!channelbag_map) {
    /* No rotation fcurves for that bone. */
    return false;
  }

  const StringRef rotation_mode_name = get_rotation_mode_path(to_mode);
  std::string new_rotation_path = pchan_path.value() + "." + rotation_mode_name;

  /* Storing the previous rotation for euler angles larger than 180 degrees. */
  float4 previous_conversion(0);
  float4 converted_rotation(0);
  float4 rotation_values(0);
  float4 rot_quat;

  const int evaluation_buffer_count = current_mode > ROT_MODE_QUAT ? 3 : 4;
  const int insertion_buffer_count = to_mode > ROT_MODE_QUAT ? 3 : 4;
  /* True if the conversion is just between different euler rotations. */
  const bool is_rotation_order_change = current_mode > ROT_MODE_QUAT && to_mode > ROT_MODE_QUAT;

  Array<FCurve *> evaluation_buffer(evaluation_buffer_count);
  Array<FCurve *> insertion_buffer(insertion_buffer_count);

  for (const auto &item : channelbag_map->items()) {
    if (is_rotation_order_change) {
      /* Cannot use the FCurve directly from the channelbag. Modifying that while converting the
       * rotation mode would influence the result. */
      FCurveDescriptor descriptor = {new_rotation_path, 0, PROP_FLOAT, PROP_EULER, pchan.name};
      BLI_assert_msg(evaluation_buffer_count == insertion_buffer_count &&
                         evaluation_buffer_count == 3,
                     "Both rotation modes are euler so should have 3 elements.");
      for (int i : IndexRange(3)) {
        descriptor.array_index = i;
        insertion_buffer[i] = &item.key->fcurve_ensure(bmain, descriptor);
        evaluation_buffer[i] = BKE_fcurve_copy(insertion_buffer[i]);
      }
    }
    else {
      for (int i : IndexRange(evaluation_buffer_count)) {
        evaluation_buffer[i] = item.value.fcurves[i];
      }
      FCurveDescriptor descriptor = {new_rotation_path, 0, PROP_FLOAT, PROP_EULER, pchan.name};
      for (int i : IndexRange(insertion_buffer_count)) {
        descriptor.array_index = i;
        insertion_buffer[i] = &item.key->fcurve_ensure(bmain, descriptor);
      }
    }
    Vector<int64_t> keyframe_ids = build_keyframe_ids(evaluation_buffer);
    get_rotation_values(pchan, rotation_values);
    KeyframeSettings settings = {BEZT_KEYTYPE_KEYFRAME, HD_AUTO_ANIM, BEZT_IPO_BEZ};
    for (FCurve *fcurve : evaluation_buffer) {
      if (!fcurve || !fcurve->bezt) {
        continue;
      }
      /* Using the settings of the first key assumes that the settings are consistent which they
       * may not be. We will need to see if this is an issue in practice. */
      BezTriple &key = fcurve->bezt[0];
      settings.handle = eBezTriple_Handle(key.h1);
      settings.interpolation = eBezTriple_Interpolation(key.ipo);
      settings.keyframe_type = BEZKEYTYPE(&key);
      break;
    }

    for (const int64_t frame_id : keyframe_ids) {
      const float frame = frame_id * BEZT_BINARYSEARCH_THRESH;
      /* Generate the current rotation values respecting missing FCurves. */
      for (FCurve *fcurve : evaluation_buffer) {
        if (!fcurve) {
          continue;
        }
        rotation_values[fcurve->array_index] = evaluate_fcurve(fcurve, frame);
      }
      /* Convert those to the new rotation mode. */
      rotation_values_to_quat(rotation_values, current_mode, rot_quat);
      quat_to_rotation_values(rot_quat, to_mode, previous_conversion, converted_rotation);
      for (int i : IndexRange(insertion_buffer_count)) {
        FCurve *fcurve = insertion_buffer[i];
        BLI_assert_msg(fcurve, "For insertion all FCurves are expected to be created before");
        insert_vert_fcurve(fcurve, {frame, converted_rotation[i]}, settings, eInsertKeyFlags(0));
      }
      previous_conversion = converted_rotation;
    }

    if (is_rotation_order_change) {
      /* Free the FCurves that have been duplicated beforehand. */
      for (FCurve *fcurve : evaluation_buffer) {
        BKE_fcurve_free(fcurve);
      }
    }
    else {
      /* When changing between euler, axis angle or quaternion the currently existing rotation
       * FCurves need to be removed. */
      for (FCurve *fcurve : evaluation_buffer) {
        item.key->fcurve_remove(*fcurve);
      }
    }
  }
  return true;
}

RNAPathFCurveMap build_rotation_fcurve_map(Action &action, const slot_handle_t slot_handle)
{
  RNAPathFCurveMap rotation_map;
  for (Channelbag *channelbag : channelbags_for_action_slot(action, slot_handle)) {
    for (FCurve *fcurve : channelbag->fcurves()) {
      StringRefNull rna_path(fcurve->rna_path);
      if (!is_rotation_path(rna_path)) {
        continue;
      }
      ChannelbagFCurveMap &rotations = rotation_map.lookup_or_add(rna_path, {});
      RotationFCurves &fcurve_group = rotations.lookup_or_add(channelbag, {});
      fcurve_group.insert_fcurve(fcurve);
    }
  }
  return rotation_map;
}

}  // namespace blender::animrig
