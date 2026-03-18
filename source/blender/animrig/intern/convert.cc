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

void SortedFCurveBuffer::insert_fcurve(FCurve &fcurve)
{
  int insert_index = 0;
  for (FCurve *existing_fcurve : fcurves_) {
    BLI_assert(fcurve.array_index != existing_fcurve->array_index);
    if (existing_fcurve->array_index > fcurve.array_index) {
      break;
    }
    insert_index++;
  }
  fcurves_.insert(insert_index, &fcurve);
}

Span<FCurve *> SortedFCurveBuffer::fcurves() const
{
  return fcurves_;
}

FCurve *SortedFCurveBuffer::get_fcurve_by_array_index(const int array_index) const
{
  for (FCurve *fcurve : fcurves_) {
    if (fcurve->array_index == array_index) {
      return fcurve;
    }
  }
  return nullptr;
}

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

/* Returns the ranges in which a rotation mode is active. Each entry denotes the starting point of
 * the range and it ends with the next entry. If the FCurve has any keys, there will be at least
 * one entry with the starting mode. */
static Vector<std::pair<float, eRotationModes>> get_rotation_mode_ranges(const FCurve &fcurve)
{
  BLI_assert(StringRefNull(fcurve.rna_path).endswith("rotation_mode"));
  if (!fcurve.bezt) {
    return {};
  }
  Vector<std::pair<float, eRotationModes>> changes;
  for (const int i : IndexRange(fcurve.totvert)) {
    const BezTriple &key = fcurve.bezt[i];
    const eRotationModes key_rotation_mode = eRotationModes(key.vec[1][1]);
    if (!changes.is_empty() && changes.last().second == key_rotation_mode) {
      continue;
    }
    changes.append({key.vec[1][0], key_rotation_mode});
  }
  return changes;
}

static void convert_fcurves_rotation_mode(Span<FCurve *> evaluation_buffer,
                                          Span<FCurve *> insertion_buffer,
                                          const eRotationModes from_mode,
                                          const eRotationModes to_mode,
                                          const float2 range,
                                          bPoseChannel &pchan)
{
  Vector<int64_t> keyframe_ids = build_keyframe_ids(evaluation_buffer);
  /* Storing the previous rotation for euler angles larger than 180 degrees. */
  float4 previous_conversion(0);
  float4 converted_rotation(0);
  float4 rotation_values(0);
  float4 rot_quat(0);
  /* Filling the array with the current values to have good base values in case not every array
   * index is keyed. */
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
    if (frame < range[0]) {
      continue;
    }
    if (frame >= range[1]) {
      break;
    }
    /* Generate the current rotation values respecting missing FCurves. */
    for (FCurve *fcurve : evaluation_buffer) {
      if (!fcurve) {
        continue;
      }
      rotation_values[fcurve->array_index] = evaluate_fcurve(fcurve, frame);
    }
    /* Convert those to the new rotation mode. */
    rotation_values_to_quat(rotation_values, from_mode, rot_quat);
    quat_to_rotation_values(rot_quat, to_mode, previous_conversion, converted_rotation);
    for (int i : insertion_buffer.index_range()) {
      FCurve *fcurve = insertion_buffer[i];
      BLI_assert_msg(fcurve, "For insertion all FCurves are expected to be created before");
      insert_vert_fcurve(fcurve, {frame, converted_rotation[i]}, settings, eInsertKeyFlags(0));
    }
    previous_conversion = converted_rotation;
  }
}

static void convert_rotation_mode_range(Main &bmain,
                                        Channelbag &channelbag,
                                        const SortedFCurveBuffer &fcurve_buffer,
                                        const StringRef rna_base_path,
                                        const eRotationModes from_mode,
                                        const eRotationModes to_mode,
                                        const float2 range,
                                        bPoseChannel &pchan)
{
  const int evaluation_buffer_count = from_mode > ROT_MODE_QUAT ? 3 : 4;
  const int insertion_buffer_count = to_mode > ROT_MODE_QUAT ? 3 : 4;

  Array<FCurve *> evaluation_buffer(evaluation_buffer_count);
  Array<FCurve *> insertion_buffer(insertion_buffer_count);

  const bool is_rotation_order_change = from_mode > ROT_MODE_QUAT && to_mode > ROT_MODE_QUAT;

  {
    const StringRef rotation_mode_name = get_rotation_mode_path(to_mode);
    const std::string new_rotation_path = rna_base_path + "." + rotation_mode_name;

    /* True if the conversion is just between different euler rotations. */

    if (is_rotation_order_change) {
      /* Cannot use the FCurve directly from the channelbag. Modifying that while converting the
       * rotation mode would influence the result. */
      FCurveDescriptor descriptor = {new_rotation_path, 0, PROP_FLOAT, PROP_EULER, pchan.name};
      BLI_assert_msg(evaluation_buffer_count == insertion_buffer_count &&
                         evaluation_buffer_count == 3,
                     "Both rotation modes are euler so should have 3 elements.");
      for (int i : IndexRange(3)) {
        descriptor.array_index = i;
        insertion_buffer[i] = &channelbag.fcurve_ensure(&bmain, descriptor);
        evaluation_buffer[i] = BKE_fcurve_copy(insertion_buffer[i]);
      }
    }
    else {
      for (int i : IndexRange(evaluation_buffer_count)) {
        evaluation_buffer[i] = fcurve_buffer.get_fcurve_by_array_index(i);
      }
      FCurveDescriptor descriptor = {new_rotation_path, 0, PROP_FLOAT, PROP_EULER, pchan.name};
      for (int i : IndexRange(insertion_buffer_count)) {
        descriptor.array_index = i;
        insertion_buffer[i] = &channelbag.fcurve_ensure(&bmain, descriptor);
      }
    }
  }

  convert_fcurves_rotation_mode(
      evaluation_buffer, insertion_buffer, from_mode, to_mode, range, pchan);

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
        channelbag.fcurve_remove(*fcurve);
      }
    }
}

bool convert_pose_bone_rotation_keys(Main *bmain,
                                     ID &owner_id,
                                     bPoseChannel &pchan,
                                     const ChannelbagToFCurveMap &channelbag_fcurve_map,
                                     const eRotationModes to_mode)
{
  PointerRNA ptr = RNA_pointer_create_discrete(&owner_id, RNA_PoseBone, &pchan);
  const std::optional<std::string> pchan_path = RNA_path_from_ID_to_struct(&ptr);
  if (!pchan_path) {
    return false;
  }

  bool modified_keys = false;

  for (const auto &item : channelbag_fcurve_map.items()) {
    Channelbag *channelbag = item.key;
    const RNAFCurveMap &fcu_map = item.value;
    const std::string rotation_mode_path = pchan_path.value() + ".rotation_mode";
    Vector<std::pair<float, eRotationModes>> rotation_mode_ranges;
    FCurve *rotation_mode_fcurve = nullptr;
    if (const SortedFCurveBuffer *rotation_mode_buffer = fcu_map.lookup_ptr(rotation_mode_path)) {
      BLI_assert(rotation_mode_buffer->fcurves().size() > 0);
      rotation_mode_fcurve = rotation_mode_buffer->fcurves()[0];
      rotation_mode_ranges = get_rotation_mode_ranges(*rotation_mode_fcurve);
    }
    else {
      /* Defaulting back to the struct value means that this can have unexpected results when
       * dealing with action layers. The rotation mode can still be animated by a higher layer but
       * that means we cannot know the correct rotation mode for the current layer. */
      rotation_mode_ranges = {{0, eRotationModes(pchan.rotmode)}};
    }

    for (const int i : rotation_mode_ranges.index_range()) {
      const std::pair<float, eRotationModes> &rotation_mode_range = rotation_mode_ranges[i];
      const eRotationModes from_mode = rotation_mode_range.second;
      const StringRef rotation_mode = get_rotation_mode_path(from_mode);
      const std::string current_rotation_path = pchan_path.value() + "." + rotation_mode;
      const SortedFCurveBuffer *rotation_fcurves = fcu_map.lookup_ptr(current_rotation_path);
      if (!rotation_fcurves) {
        continue;
      }
      float2 range(rotation_mode_range.first, FLT_MAX);
      if (i + 1 < rotation_mode_ranges.size()) {
        range[1] = rotation_mode_ranges[i + 1].first;
      }
      convert_rotation_mode_range(*bmain,
                                  *channelbag,
                                  *rotation_fcurves,
                                  pchan_path.value(),
                                  from_mode,
                                  to_mode,
                                  range,
                                  pchan);
      modified_keys = true;
    }
    if (rotation_mode_fcurve && rotation_mode_fcurve->bezt) {
      for (const int i : IndexRange(rotation_mode_fcurve->totvert)) {
        rotation_mode_fcurve->bezt[i].vec[1][1] = to_mode;
      }
      BKE_fcurve_handles_recalc(*rotation_mode_fcurve);
    }
  }

  return modified_keys;
}

static bool is_rotation_mode_path(const StringRefNull rna_path)
{
  const int start_of_propname = rna_path.rfind(".") + 1;
  return rna_path.substr(start_of_propname, rna_path.size()) == "rotation_mode";
}

ChannelbagToFCurveMap build_rotation_fcurve_map(Action &action, const slot_handle_t slot_handle)
{
  ChannelbagToFCurveMap rotation_map;
  for (Channelbag *channelbag : channelbags_for_action_slot(action, slot_handle)) {
    RNAFCurveMap &curves = rotation_map.lookup_or_add(channelbag, {});
    for (FCurve *fcurve : channelbag->fcurves()) {
      StringRefNull rna_path(fcurve->rna_path);
      if (!is_rotation_path(rna_path) && !is_rotation_mode_path(rna_path)) {
        continue;
      }
      SortedFCurveBuffer &fcurve_buffer = curves.lookup_or_add(rna_path, {});
      fcurve_buffer.insert_fcurve(*fcurve);
    }
  }
  return rotation_map;
}

}  // namespace blender::animrig
