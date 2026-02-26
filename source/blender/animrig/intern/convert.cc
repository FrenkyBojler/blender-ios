#include "DNA_action_types.h"
#include "DNA_anim_types.h"
#include "DNA_object_types.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "BLI_math_rotation.h"
#include "BLI_set.hh"
#include "BLI_math_vector.h"

#include "BKE_fcurve.hh"

#include "ANIM_rna.hh"
#include "ANIM_fcurve.hh"
#include "ANIM_convert.hh"
#include "ANIM_action.hh"

namespace blender::animrig{

    /* Builds a set of frames where at least one of the given FCurves has a key. This uses int instead
 * of float to avoid precision issues. The maximum subframe resolution is dicated by
 * BEZT_BINARYSEARCH_THRESH so this is used to convert to a unique integer. */
static Set<int64_t> build_keyframe_ids(FCurve *fcurves[4])
{
  Set<int64_t> keyframe_ids;
  const Span<FCurve *> fcurve_span(fcurves, 4);
  for (FCurve *fcurve : fcurve_span) {
    if (!fcurve || !fcurve->bezt) {
      continue;
    }
    for (int i = 0; i < fcurve->totvert; i++) {
      keyframe_ids.add(int64_t(fcurve->bezt[i].vec[1][0] / BEZT_BINARYSEARCH_THRESH));
    }
  }
  return keyframe_ids;
}

    static void rotation_values_to_matrix(const float rotation_values[4],
                                      const eRotationModes mode,
                                      float r_matrix[3][3])
{
  switch (mode) {
    case ROT_MODE_QUAT:
      quat_to_mat3(r_matrix, rotation_values);
      break;

    case ROT_MODE_AXISANGLE:
      axis_angle_to_mat3(r_matrix, &rotation_values[1], rotation_values[0]);
      break;

    default:
      eulO_to_mat3(r_matrix, rotation_values, mode);
      break;
  }
}

static void matrix_to_rotation_values(const float matrix[3][3],
                                      const eRotationModes mode,
                                      const float reference_rotation[4],
                                      float r_rotation_values[4])
{
  switch (mode) {
    case ROT_MODE_QUAT:
      mat3_to_quat(r_rotation_values, matrix);
      break;

    case ROT_MODE_AXISANGLE:
      mat3_to_axis_angle(&r_rotation_values[1], r_rotation_values, matrix);
      break;

    default:
      mat3_to_compatible_eulO(r_rotation_values, reference_rotation, mode, matrix);
      break;
  }
}

static void get_rotation_values(const bPoseChannel &pose_bone, float rotation_values[4])
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

void convert_pose_bone_rotation_keys(Main *bmain,
                                            Object &ob,
                                            bPoseChannel &pchan,
                                            RNAPathFCurveMap &fcurves_by_rna_path,
                                            const eRotationModes to_mode)
{
  PointerRNA ptr = RNA_pointer_create_discrete(&ob.id, RNA_PoseBone, &pchan);
  const std::optional<std::string> pchan_path = RNA_path_from_ID_to_struct(&ptr);
  if (!pchan_path) {
    return;
  }
  const StringRef rotation_mode = get_rotation_mode_path(eRotationModes(pchan.rotmode));
  /* This is the current rotation mode path. */
  std::string current_rotation_path = pchan_path.value() + "." + rotation_mode;
  ChannelbagFCurveMap *channelbag_map = fcurves_by_rna_path.lookup_ptr(current_rotation_path);
  if (!channelbag_map) {
    /* No rotation fcurves for that bone. */
    return;
  }

  const StringRef rotation_mode_name = animrig::get_rotation_mode_path(to_mode);
  std::string new_rotation_path = pchan_path.value() + "." + rotation_mode_name;

  /* Storing the previous rotation for euler angles larger than 180 degrees. */
  float previous_conversion[4] = {0, 0, 0, 0};
  float converted_rotation[4];
  float rotation_values[4];
  float rotation_matrix[3][3];

  const int evaluation_buffer_count = pchan.rotmode > ROT_MODE_QUAT ? 3 : 4;
  const int insertion_buffer_count = to_mode > ROT_MODE_QUAT ? 3 : 4;
  /* True if the conversion is just between different euler rotations. */
  const bool is_rotation_order_change = pchan.rotmode > ROT_MODE_QUAT && to_mode > ROT_MODE_QUAT;
  FCurve *evaluation_buffer[4];
  FCurve *insertion_buffer[4];

  for (const auto &item : channelbag_map->items()) {

    if (is_rotation_order_change) {
      /* Cannot use the FCurve directly from the channelbag. Modifying that while converting the
       * rotation mode could influence the result. */
      animrig::FCurveDescriptor descriptor = {
          new_rotation_path, 0, PROP_FLOAT, PROP_EULER, pchan.name};
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
      animrig::FCurveDescriptor descriptor = {
          new_rotation_path, 0, PROP_FLOAT, PROP_EULER, pchan.name};
      for (int i : IndexRange(insertion_buffer_count)) {
        descriptor.array_index = i;
        insertion_buffer[i] = &item.key->fcurve_ensure(bmain, descriptor);
      }
    }
    Set<int64_t> keyframe_ids = build_keyframe_ids(evaluation_buffer);
    get_rotation_values(pchan, rotation_values);
    KeyframeSettings settings = {BEZT_KEYTYPE_KEYFRAME, HD_AUTO_ANIM, BEZT_IPO_BEZ};
    for (int i : IndexRange(evaluation_buffer_count)) {
      FCurve *fcurve = evaluation_buffer[i];
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
      for (int i : IndexRange(evaluation_buffer_count)) {
        FCurve *fcurve = evaluation_buffer[i];
        if (!fcurve) {
          continue;
        }
        rotation_values[fcurve->array_index] = evaluate_fcurve(fcurve, frame);
      }
      /* Convert those to the new rotation mode. */
      rotation_values_to_matrix(rotation_values, eRotationModes(pchan.rotmode), rotation_matrix);
      matrix_to_rotation_values(rotation_matrix, to_mode, previous_conversion, converted_rotation);
      for (int i : IndexRange(insertion_buffer_count)) {
        /* Insert a key */
        FCurve *fcurve = insertion_buffer[i];
        BLI_assert_msg(fcurve, "For insertion all FCurves are expected to be created before");
        insert_vert_fcurve(
            fcurve, {frame, converted_rotation[i]}, settings, eInsertKeyFlags(0));
      }

      std::swap(converted_rotation, previous_conversion);
    }

    if (is_rotation_order_change) {
      /* Free the FCurves that have been duplicated beforehand. */
      for (int i = 0; i < evaluation_buffer_count; i++) {
        BKE_fcurve_free(evaluation_buffer[i]);
      }
    }
    else {
      /* When changing between euler, axis angle or quaternion the currently existing rotation
       * FCurves need to be removed. */
      for (int i : IndexRange(evaluation_buffer_count)) {
        item.key->fcurve_remove(*evaluation_buffer[i]);
      }
    }
  }
}
}