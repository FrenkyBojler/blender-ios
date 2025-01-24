/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup fbx
 */

#include <algorithm>

#include "ANIM_action.hh"
#include "ANIM_animdata.hh"

#include "BKE_action.hh"
#include "BKE_fcurve.hh"
#include "BKE_lib_id.hh"

#include "BLI_math_rotation.h"
#include "BLI_set.hh"
#include "BLI_string.h"

#include "fbx_import_anim.hh"

#include "ufbx.h"

namespace blender::io::fbx {

/**
 * Ensures that the given ID has an action assigned to it and, for layered
 * actions, an assigned slot.
 */
static bAction *ensure_action_and_slot_for_id(Main &bmain, ID &id, StringRefNull action_name)
{
  bAction *baction = animrig::id_action_ensure(&bmain, &id);
  BLI_assert(baction != nullptr);
  BKE_id_rename(bmain, baction->id, action_name);

  animrig::Action &action = baction->wrap();
  BLI_assert(action.is_action_layered());
  animrig::Slot *slot = animrig::assign_action_ensure_slot_for_keying(action, id);
  BLI_assert(slot != nullptr);

  const std::string slot_name = slot->identifier_prefix_for_idtype() + action_name;
  action.slot_identifier_define(*slot, slot_name);

  return baction;
}

static FCurve *create_fcurve(const std::string &rna_path, int array_index, int64_t key_count)
{
  FCurve *cu = BKE_fcurve_create();
  cu->flag = FCURVE_VISIBLE | FCURVE_SELECTED;
  cu->auto_smoothing = U.auto_smoothing_new;
  cu->rna_path = BLI_strdup(rna_path.c_str());
  cu->array_index = array_index;
  cu->bezt = MEM_cnew_array<BezTriple>(key_count, "beztriple");
  cu->totvert = key_count;
  return cu;
}

static void set_curve_sample(FCurve *curve, int64_t key_index, float time, float value)
{
  BLI_assert(key_index >= 0 && key_index < curve->totvert);
  BezTriple &bez = curve->bezt[key_index];
  bez.vec[1][0] = time;
  bez.vec[1][1] = value;
  bez.ipo = BEZT_IPO_LIN;
  bez.f1 = bez.f2 = bez.f3 = SELECT;
  bez.h1 = bez.h2 = HD_AUTO_ANIM;
}

struct ElementAnimations {
  const ufbx_anim_stack *fbx_stack = nullptr;
  const ufbx_anim_layer *fbx_layer = nullptr;
  Object *target_obj = nullptr;
  Vector<const ufbx_anim_prop *> props;
  const ufbx_anim_prop *prop_position = nullptr;
  const ufbx_anim_prop *prop_rotation = nullptr;
  const ufbx_anim_prop *prop_scale = nullptr;
};

static Map<const ufbx_element *, ElementAnimations> gather_animated_properties(
    const ufbx_scene &fbx, const Map<const ufbx_element *, Object *> &element_to_object)
{
  Map<const ufbx_element *, ElementAnimations> animations;
  for (const ufbx_anim_stack *fstack : fbx.anim_stacks) {
    for (const ufbx_anim_layer *flayer : fstack->layers) {
      // printf("fbx: AnimLayer %s\n", flayer->name.data);
      for (const ufbx_anim_prop &fprop : flayer->anim_props) {
        // printf("fbx: - prop '%s' el '%s'\n", fprop.prop_name.data, fprop.element->name.data);
        bool supported_prop = false;
        //@TODO: "DeformPercent" - shape keys, element is keyblock
        //@TODO: "FocalLength", "FocusDistance" - element is camera
        //@TODO: "DiffuseColor" - element is material
        //@TODO: "Visibility"?
        const bool is_position = STREQ(fprop.prop_name.data, "Lcl Translation");
        const bool is_rotation = STREQ(fprop.prop_name.data, "Lcl Rotation");
        const bool is_scale = STREQ(fprop.prop_name.data, "Lcl Scale");
        if (is_position || is_rotation || is_scale) {
          supported_prop = true;
        }

        if (!supported_prop) {
          continue;
        }

        //@TODO: make this handle non-Object animation (e.g. Materials)
        Object *target_obj = element_to_object.lookup_default(fprop.element, nullptr);
        if (target_obj == nullptr) {
          continue;
        }

        ElementAnimations &anims = animations.lookup_or_add(fprop.element, ElementAnimations());
        if (anims.fbx_stack == nullptr) {
          anims.fbx_stack = fstack;
        }
        if (anims.fbx_layer == nullptr) {
          anims.fbx_layer = flayer;
        }
        anims.target_obj = target_obj;
        if (is_position) {
          anims.prop_position = &fprop;
        }
        if (is_rotation) {
          anims.prop_rotation = &fprop;
        }
        if (is_scale) {
          anims.prop_scale = &fprop;
        }

        anims.props.append(&fprop);
      }
    }
  }
  return animations;
}

static bAction *create_action(Main &bmain,
                              const ElementAnimations &anim,
                              Map<std::string, bAction *> &action_name_map)
{
  /* Construct action name. */
  std::string action_name = BKE_id_name(anim.target_obj->id);
  action_name += '|';
  action_name += anim.fbx_stack->name.data;
  if (!STREQ(anim.fbx_stack->name.data, anim.fbx_layer->name.data)) {
    action_name += '|';
    action_name += anim.fbx_layer->name.data;
  }

  /* Lookup or create an action. */
  bAction *action = action_name_map.lookup_default(action_name, nullptr);
  if (action == nullptr) {
    action = ensure_action_and_slot_for_id(bmain, anim.target_obj->id, action_name);
    action_name_map.add_new(action_name, action);
  }
  BLI_assert(anim.target_obj->adt != nullptr);

  return action;
}

static void create_transform_curves(const ufbx_element *item_key,
                                    const ElementAnimations &anim,
                                    bAction *action,
                                    const double fps,
                                    const float anim_offset,
                                    const double unit_scale)
{
  std::string rna_position = "location";

  std::string rna_rotation;
  int rot_channels = 3;
  const eRotationModes rot_mode = static_cast<eRotationModes>(anim.target_obj->rotmode);
  switch (rot_mode) {
    case ROT_MODE_QUAT:
      rna_rotation = "rotation_quaternion";
      rot_channels = 4;
      break;
    case ROT_MODE_AXISANGLE:
      rna_rotation = "rotation_axis_angle";
      rot_channels = 4;
      break;
    default:
      rna_rotation = "rotation_euler";
      rot_channels = 3;
      break;
  }

  std::string rna_scale = "scale";

  /* Note: Python importer was always creating all pos/rot/scale curves: "due to all FBX
   * transform magic, we need to add curves for whole loc/rot/scale in any case".
   *
   * Also, we create a full transform keyframe at any point where input pos/rot/scale curves have
   * a keyframe. It should not be needed if we fully imported curves with all their proper
   * handles, but again currently this is to match Python importer behavior. */
  const ufbx_anim_curve *input_curves[9] = {};
  if (anim.prop_position) {
    input_curves[0] = anim.prop_position->anim_value->curves[0];
    input_curves[1] = anim.prop_position->anim_value->curves[1];
    input_curves[2] = anim.prop_position->anim_value->curves[2];
  }
  if (anim.prop_rotation) {
    input_curves[3] = anim.prop_rotation->anim_value->curves[0];
    input_curves[4] = anim.prop_rotation->anim_value->curves[1];
    input_curves[5] = anim.prop_rotation->anim_value->curves[2];
  }
  if (anim.prop_scale) {
    input_curves[6] = anim.prop_scale->anim_value->curves[0];
    input_curves[7] = anim.prop_scale->anim_value->curves[1];
    input_curves[8] = anim.prop_scale->anim_value->curves[2];
  }

  /* Figure out timestamps of where any of input curves have a keyframe. */
  Set<double> unique_key_times;
  for (int i = 0; i < 9; i++) {
    if (input_curves[i] != nullptr) {
      for (const ufbx_keyframe &key : input_curves[i]->keyframes) {
        if (key.interpolation == UFBX_INTERPOLATION_CUBIC) {
          /* Hack: force cubic keyframes to be linear, to match Python importer behavior. */
          const_cast<ufbx_keyframe &>(key).interpolation = UFBX_INTERPOLATION_LINEAR;
        }
        unique_key_times.add(key.time);
      }
    }
  }
  Vector<double> sorted_key_times(unique_key_times.begin(), unique_key_times.end());
  std::sort(sorted_key_times.begin(), sorted_key_times.end());

  /* Create all the f-curves. */
  FCurve *curves_pos[3] = {};
  for (int i = 0; i < 3; i++) {
    curves_pos[i] = create_fcurve(rna_position, i, sorted_key_times.size());
  }
  FCurve *curves_rot[4] = {};
  for (int i = 0; i < rot_channels; i++) {
    curves_rot[i] = create_fcurve(rna_rotation, i, sorted_key_times.size());
  }
  FCurve *curves_scale[3] = {};
  for (int i = 0; i < 3; i++) {
    curves_scale[i] = create_fcurve(rna_scale, i, sorted_key_times.size());
  }

  /* Evaluate transforms at all the key times. */
  for (int64_t i = 0; i < sorted_key_times.size(); i++) {
    double t = sorted_key_times[i];
    float tf = float(t * fps + anim_offset);
    ufbx_transform xform = ufbx_evaluate_transform(
        anim.fbx_layer->anim, (const ufbx_node *)item_key, t);
    set_curve_sample(curves_pos[0], i, tf, float(xform.translation.x * unit_scale));
    set_curve_sample(curves_pos[1], i, tf, float(xform.translation.y * unit_scale));
    set_curve_sample(curves_pos[2], i, tf, float(xform.translation.z * unit_scale));

    float4 quat(xform.rotation.x, xform.rotation.y, xform.rotation.z, xform.rotation.w);
    switch (rot_mode) {
      case ROT_MODE_QUAT:
        set_curve_sample(curves_rot[0], i, tf, quat.x);
        set_curve_sample(curves_rot[1], i, tf, quat.y);
        set_curve_sample(curves_rot[2], i, tf, quat.z);
        set_curve_sample(curves_rot[3], i, tf, quat.w);
        break;
      case ROT_MODE_AXISANGLE: {
        float3 axis;
        float angle;
        quat_to_axis_angle(axis, &angle, quat);
        set_curve_sample(curves_rot[0], i, tf, angle);
        set_curve_sample(curves_rot[1], i, tf, axis.x);
        set_curve_sample(curves_rot[2], i, tf, axis.y);
        set_curve_sample(curves_rot[3], i, tf, axis.z);
      } break;
      default: {
        float3 euler;
        quat_to_eul(euler, quat);
        set_curve_sample(curves_rot[0], i, tf, euler.x);
        set_curve_sample(curves_rot[1], i, tf, euler.y);
        set_curve_sample(curves_rot[2], i, tf, euler.z);
      } break;
    }

    set_curve_sample(curves_scale[0], i, tf, float(xform.scale.x));
    set_curve_sample(curves_scale[1], i, tf, float(xform.scale.y));
    set_curve_sample(curves_scale[2], i, tf, float(xform.scale.z));
  }

  /* Finalize and attach the curves. */
  auto finalize_curve = [action, anim](FCurve *cu) {
    if (cu != nullptr) {
      BKE_fcurve_handles_recalc(cu);
      blender::animrig::action_fcurve_attach(
          action->wrap(), anim.target_obj->adt->slot_handle, *cu, std::nullopt);
    }
  };
  for (FCurve *cu : curves_pos) {
    finalize_curve(cu);
  }
  for (FCurve *cu : curves_rot) {
    finalize_curve(cu);
  }
  for (FCurve *cu : curves_scale) {
    finalize_curve(cu);
  }
}

void import_animations(Main &bmain,
                       const ufbx_scene &fbx,
                       const Map<const ufbx_element *, Object *> &element_to_object,
                       const double fps,
                       const float anim_offset)
{
  /* Note: mixing is completely ignored for now, each layer results in an independent set of
   * actions. */

  Map<const ufbx_element *, ElementAnimations> animations = gather_animated_properties(
      fbx, element_to_object);

  Map<std::string, bAction *> action_name_map;
  for (const auto &item : animations.items()) {
    ElementAnimations &anim = item.value;
    bAction *action = create_action(bmain, anim, action_name_map);

    if (anim.prop_position || anim.prop_rotation || anim.prop_scale) {
      create_transform_curves(item.key, anim, action, fps, anim_offset, fbx.settings.unit_meters);
    }
  }
}

}  // namespace blender::io::fbx
