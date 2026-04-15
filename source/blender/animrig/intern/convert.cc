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

#include "ED_transformable.hh"

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

/**
 * For all keyframes in the `evaluation_buffer` in the given range, insert values into
 * `insertion_buffer` that represent the same rotation but in the given rotation mode.
 *
 * \param evaluation_buffer It is assumed that those FCurves match the rotation mode `from_mode`.
 * They will be read for keyframe values. It is allowed to have nullptr FCurves in here.
 * \param insertion_buffer The FCurves relating to `to_mode`. Keyframes for the converted rotation
 * mode will be inserted here. None of the FCurves shall be a nullptr.
 * \param range Start and end frames to limit the range in which to convert and insert rotation
 * keys. Interpreted inclusive at the start and exclusive at the end and in FCurve space.
 */
static void convert_fcurves_rotation_mode(const Span<const FCurve *> evaluation_buffer,
                                          const Span<FCurve *> insertion_buffer,
                                          const eRotationModes from_mode,
                                          const eRotationModes to_mode,
                                          const float2 range,
                                          const Transformable &transformable)
{
  /* Filling the array with the current values to have good base values in case not every array
   * index is keyed. */
  Rotation rotation_values = transformable.get_rotation_for_mode(from_mode);
  KeyframeSettings settings;
  for (const FCurve *fcurve : evaluation_buffer) {
    if (!fcurve || !fcurve->bezt) {
      continue;
    }
    /* Using the settings of the first key assumes that the settings are consistent which they
     * may not be. We will need to see if this is an issue in practice. */
    const BezTriple &key = fcurve->bezt[0];
    settings.handle = eBezTriple_Handle(key.h1);
    settings.interpolation = eBezTriple_Interpolation(key.ipo);
    settings.keyframe_type = BEZKEYTYPE(&key);
    break;
  }

  /* Storing the previous rotation for euler angles larger than 180 degrees. */
  Rotation previous_conversion = rotation_values;

  Vector<int64_t> keyframe_ids = build_keyframe_ids(evaluation_buffer);
  for (const int64_t frame_id : keyframe_ids) {
    const float frame = frame_id * BEZT_BINARYSEARCH_THRESH;
    if (frame < range[0]) {
      continue;
    }
    if (frame >= range[1]) {
      break;
    }
    /* Generate the current rotation values respecting missing FCurves. */
    for (const FCurve *fcurve : evaluation_buffer) {
      if (!fcurve) {
        continue;
      }
      rotation_values.values[fcurve->array_index] = evaluate_fcurve(fcurve, frame);
    }
    Rotation converted_rotation = rotation_values.converted_to_mode(to_mode, &previous_conversion);
    for (int i : insertion_buffer.index_range()) {
      FCurve *fcurve = insertion_buffer[i];
      BLI_assert_msg(fcurve, "For insertion all FCurves are expected to be created before");
      insert_vert_fcurve(fcurve, {frame, converted_rotation.values[i]}, settings, INSERTKEY_FAST);
    }
    previous_conversion = converted_rotation;
  }

  for (FCurve *fcurve : insertion_buffer) {
    BKE_fcurve_handles_recalc(*fcurve);
  }
}

static void convert_rotation_mode_range(Main &bmain,
                                        Channelbag &channelbag,
                                        const SortedFCurveBuffer &fcurve_buffer,
                                        const eRotationModes from_mode,
                                        const eRotationModes to_mode,
                                        const float2 range,
                                        const Transformable &transformable)
{
  const int evaluation_buffer_count = from_mode > ROT_MODE_QUAT ? 3 : 4;
  const int insertion_buffer_count = to_mode > ROT_MODE_QUAT ? 3 : 4;

  Array<FCurve *> evaluation_buffer(evaluation_buffer_count);
  Array<FCurve *> insertion_buffer(insertion_buffer_count);

  const bool is_euler_to_euler = from_mode > ROT_MODE_QUAT && to_mode > ROT_MODE_QUAT;

  {
    const std::string to_mode_rna_path = transformable.rna_path_to_rotation_mode(to_mode);

    if (is_euler_to_euler) {
      /* Cannot use the FCurve directly from the channelbag. Modifying that while converting the
       * rotation mode would influence the result. */
      FCurveDescriptor descriptor = {
          to_mode_rna_path, 0, PROP_FLOAT, PROP_EULER, transformable.fcurve_group_name()};
      BLI_assert_msg(evaluation_buffer_count == insertion_buffer_count &&
                         evaluation_buffer_count == 3,
                     "Both rotation modes are euler so should have 3 elements.");
      for (const int i : IndexRange(3)) {
        descriptor.array_index = i;
        insertion_buffer[i] = &channelbag.fcurve_ensure(&bmain, descriptor);
        evaluation_buffer[i] = BKE_fcurve_copy(insertion_buffer[i]);
      }
    }
    else {
      for (const int i : IndexRange(evaluation_buffer_count)) {
        evaluation_buffer[i] = fcurve_buffer.get_fcurve_by_array_index(i);
      }
      /* Is needed to get correct FCurve colors. */
      PropertySubType prop_subtype = PROP_EULER;
      if (to_mode == ROT_MODE_QUAT) {
        prop_subtype = PROP_QUATERNION;
      }
      else if (to_mode == ROT_MODE_AXISANGLE) {
        prop_subtype = PROP_AXISANGLE;
      }
      FCurveDescriptor descriptor = {
          to_mode_rna_path, 0, PROP_FLOAT, prop_subtype, transformable.fcurve_group_name()};
      for (const int i : IndexRange(insertion_buffer_count)) {
        descriptor.array_index = i;
        insertion_buffer[i] = &channelbag.fcurve_ensure(&bmain, descriptor);
      }
    }
  }

  convert_fcurves_rotation_mode(
      evaluation_buffer, insertion_buffer, from_mode, to_mode, range, transformable);

  if (is_euler_to_euler) {
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
                                     const Transformable &transformable,
                                     const ChannelbagToFCurveMap &channelbag_fcurve_map,
                                     const eRotationModes to_mode)
{
  bool modified_keys = false;

  for (const auto &item : channelbag_fcurve_map.items()) {
    Channelbag *channelbag = item.key;
    const RNAFCurveMap &fcu_map = item.value;
    const std::string rotation_mode_path = fmt::format(
        "{}.{}", transformable.rna_path(), "rotation_mode");
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
      rotation_mode_ranges = {{0, transformable.get_rotation_mode()}};
    }

    for (const int i : rotation_mode_ranges.index_range()) {
      const std::pair<float, eRotationModes> &rotation_mode_range = rotation_mode_ranges[i];
      const eRotationModes from_mode = rotation_mode_range.second;
      const std::string from_mode_rna_path = transformable.rna_path_to_rotation_mode(from_mode);
      const SortedFCurveBuffer *rotation_fcurves = fcu_map.lookup_ptr(from_mode_rna_path);
      if (!rotation_fcurves) {
        continue;
      }
      float2 range(rotation_mode_range.first, FLT_MAX);
      if (i + 1 < rotation_mode_ranges.size()) {
        range[1] = rotation_mode_ranges[i + 1].first;
      }
      convert_rotation_mode_range(
          *bmain, *channelbag, *rotation_fcurves, from_mode, to_mode, range, transformable);
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

void bake_rotation_fcurves(const ChannelbagToFCurveMap &channelbag_fcurve_map,
                           const Transformable &transformable)
{
  /* Need to bake on all potential FCurves to cover. */
  const Array<eRotationModes> rotation_modes = {ROT_MODE_EUL, ROT_MODE_QUAT, ROT_MODE_AXISANGLE};
  for (const eRotationModes rotation_mode : rotation_modes) {
    std::string rotation_rna_path = transformable.rna_path_to_rotation_mode(rotation_mode);

    for (const RNAFCurveMap &rna_fcurve_map : channelbag_fcurve_map.values()) {
      const SortedFCurveBuffer *fcurve_buffer = rna_fcurve_map.lookup_ptr(rotation_rna_path);
      if (!fcurve_buffer) {
        continue;
      }
      for (FCurve *fcurve : fcurve_buffer->fcurves()) {
        if (!fcurve || !fcurve->bezt) {
          continue;
        }
        float2 range;
        BKE_fcurve_calc_range(fcurve, &range[0], &range[1], false);
        bake_fcurve(fcurve, int2(range), 1, BakeCurveRemove::ALL);
      }
    }
  }
}

}  // namespace blender::animrig
