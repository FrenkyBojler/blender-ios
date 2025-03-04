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

#include "BLI_math_quaternion.hh"
#include "BLI_set.hh"
#include "BLI_string.h"

#include "DNA_key_types.h"

#include "fbx_import_anim.hh"
#include "fbx_import_util.hh"

namespace blender::io::fbx {

/**
 * Ensures that the given ID has an action assigned to it and, for layered
 * actions, an assigned slot.
 */
static bAction *ensure_action_and_slot_for_id(Main &bmain, ID &id, StringRefNull action_name)
{
  bAction *act = animrig::id_action_ensure(&bmain, &id);
  BLI_assert(act != nullptr);
  BKE_id_rename(bmain, act->id, action_name);

  animrig::action_channelbag_ensure(*act, id);

  animrig::Action &action = act->wrap();
  animrig::Slot *slot = animrig::assign_action_ensure_slot_for_keying(action, id);
  BLI_assert(slot != nullptr);
  const std::string slot_name = slot->idtype_string() + action_name;
  action.slot_identifier_define(*slot, slot_name);

  return act;
}

static FCurve *create_fcurve(animrig::Channelbag &channelbag,
                             const animrig::FCurveDescriptor &descriptor,
                             int64_t key_count)
{
  FCurve *cu = channelbag.fcurve_create_unique(nullptr, descriptor);
  BLI_assert_msg(cu, "The same F-Curve is being created twice, this is unexpected.");
  BKE_fcurve_bezt_resize(cu, key_count);
  memset(cu->bezt, 0, key_count * sizeof(cu->bezt[0])); /* @TODO: remove once #135448 lands */
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
  const ufbx_element *fbx_elem = nullptr;
  ID *target_id = nullptr;
  Object *target_object = nullptr;
  int64_t order = 0;
  const ufbx_anim_prop *prop_position = nullptr;
  const ufbx_anim_prop *prop_rotation = nullptr;
  const ufbx_anim_prop *prop_scale = nullptr;
  const ufbx_anim_prop *prop_blend_shape = nullptr;
};

static Vector<ElementAnimations> gather_animated_properties(const ufbx_scene &fbx,
                                                            const FbxElementMapping &mapping)
{
  int64_t order = 0;
  Map<const ufbx_element *, ElementAnimations> elem_map;
  for (const ufbx_anim_stack *fstack : fbx.anim_stacks) {
    for (const ufbx_anim_layer *flayer : fstack->layers) {
      for (const ufbx_anim_prop &fprop : flayer->anim_props) {
        bool supported_prop = false;
        //@TODO: "FocalLength", "FocusDistance" - element is camera
        //@TODO: "DiffuseColor" - element is material
        //@TODO: "Visibility"?
        const bool is_position = STREQ(fprop.prop_name.data, "Lcl Translation");
        const bool is_rotation = STREQ(fprop.prop_name.data, "Lcl Rotation");
        const bool is_scale = STREQ(fprop.prop_name.data, "Lcl Scale");
        const bool is_blend_shape = STREQ(fprop.prop_name.data, "DeformPercent");
        if (is_position || is_rotation || is_scale || is_blend_shape) {
          supported_prop = true;
        }

        if (!supported_prop) {
          continue;
        }

        //@TODO: make this handle non-Object animation (e.g. Materials)
        Object *target_obj = mapping.el_to_object.lookup_default(fprop.element, nullptr);
        Key *target_key = mapping.el_to_shape_key.lookup_default(fprop.element, nullptr);
        if (target_obj == nullptr && target_key == nullptr) {
          continue;
        }

        ElementAnimations &anims = elem_map.lookup_or_add(fprop.element, ElementAnimations());
        anims.fbx_elem = fprop.element;
        anims.order = order++;
        if (anims.fbx_stack == nullptr) {
          anims.fbx_stack = fstack;
        }
        if (anims.fbx_layer == nullptr) {
          anims.fbx_layer = flayer;
        }

        if (target_obj != nullptr) {
          anims.target_id = &target_obj->id;
          anims.target_object = target_obj;
        }
        else if (target_key != nullptr) {
          anims.target_id = &target_key->id;
        }
        if (is_position) {
          anims.prop_position = &fprop;
        }
        if (is_rotation) {
          anims.prop_rotation = &fprop;
        }
        if (is_scale) {
          anims.prop_scale = &fprop;
        }
        if (is_blend_shape) {
          anims.prop_blend_shape = &fprop;
        }
      }
    }
  }

  /* Sort returned result in the original fbx file order. */
  Vector<ElementAnimations> animations(elem_map.values().begin(), elem_map.values().end());
  std::sort(
      animations.begin(),
      animations.end(),
      [](const ElementAnimations &a, const ElementAnimations &b) { return a.order < b.order; });
  return animations;
}

static bAction *create_action(Main &bmain,
                              const ElementAnimations &anim,
                              Map<std::string, bAction *> &action_name_map)
{
  /* Construct action name. */
  BLI_assert(anim.target_id != nullptr);
  std::string action_name = BKE_id_name(*anim.target_id);
  action_name += '|';
  action_name += anim.fbx_stack->name.data;
  if (!STREQ(anim.fbx_stack->name.data, anim.fbx_layer->name.data)) {
    action_name += '|';
    action_name += anim.fbx_layer->name.data;
  }

  /* Lookup or create an action. */
  bAction *action = action_name_map.lookup_default(action_name, nullptr);
  if (action == nullptr) {
    action = ensure_action_and_slot_for_id(bmain, *anim.target_id, action_name);
    action_name_map.add_new(action_name, action);
  }
  BLI_assert(BKE_animdata_from_id(anim.target_id) != nullptr);

  return action;
}

static void finalize_curve(bAction *action, const ElementAnimations &anim, FCurve *cu)
{
  if (cu != nullptr) {
    BKE_fcurve_handles_recalc(cu);
  }
}

static void create_transform_curves(const ElementAnimations &anim,
                                    bAction *action,
                                    animrig::Channelbag &channelbag,
                                    const double fps,
                                    const float anim_offset)
{
  /* For animated bones, prepend bone path to animation curve path. */
  std::string rna_prefix;
  bool is_bone = false;
  const char *group_name = get_fbx_name(anim.fbx_elem->name);
  const ufbx_node *fnode = ufbx_as_node(anim.fbx_elem);
  if (fnode != nullptr && fnode->bone != nullptr) {
    is_bone = true;
    group_name = get_fbx_name(fnode->name, "Bone");
    rna_prefix = std::string("pose.bones[\"") + group_name + "\"].";
  }

  std::string rna_position = rna_prefix + "location";

  std::string rna_rotation;
  int rot_channels = 3;
  /* Bones are created with quaternion rotation by default. */
  eRotationModes rot_mode = is_bone || anim.target_object == nullptr ?
                                ROT_MODE_QUAT :
                                static_cast<eRotationModes>(anim.target_object->rotmode);

  switch (rot_mode) {
    case ROT_MODE_QUAT:
      rna_rotation = rna_prefix + "rotation_quaternion";
      rot_channels = 4;
      break;
    case ROT_MODE_AXISANGLE:
      rna_rotation = rna_prefix + "rotation_axis_angle";
      rot_channels = 4;
      break;
    default:
      rna_rotation = rna_prefix + "rotation_euler";
      rot_channels = 3;
      break;
  }

  std::string rna_scale = rna_prefix + "scale";

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
    curves_pos[i] = create_fcurve(
        channelbag, {rna_position, i, {}, group_name}, sorted_key_times.size());
  }
  FCurve *curves_rot[4] = {};
  for (int i = 0; i < rot_channels; i++) {
    curves_rot[i] = create_fcurve(
        channelbag, {rna_rotation, i, {}, group_name}, sorted_key_times.size());
  }
  FCurve *curves_scale[3] = {};
  for (int i = 0; i < 3; i++) {
    curves_scale[i] = create_fcurve(
        channelbag, {rna_scale, i, {}, group_name}, sorted_key_times.size());
  }

  /* Evaluate transforms at all the key times. */
  for (int64_t i = 0; i < sorted_key_times.size(); i++) {
    double t = sorted_key_times[i];
    float tf = float(t * fps + anim_offset);
    ufbx_transform xform = ufbx_evaluate_transform(anim.fbx_layer->anim, fnode, t);
    set_curve_sample(curves_pos[0], i, tf, float(xform.translation.x));
    set_curve_sample(curves_pos[1], i, tf, float(xform.translation.y));
    set_curve_sample(curves_pos[2], i, tf, float(xform.translation.z));

    math::Quaternion quat(xform.rotation.w, xform.rotation.x, xform.rotation.y, xform.rotation.z);
    switch (rot_mode) {
      case ROT_MODE_QUAT:
        set_curve_sample(curves_rot[0], i, tf, quat.w);
        set_curve_sample(curves_rot[1], i, tf, quat.x);
        set_curve_sample(curves_rot[2], i, tf, quat.y);
        set_curve_sample(curves_rot[3], i, tf, quat.z);
        break;
      case ROT_MODE_AXISANGLE: {
        math::AxisAngle axis_angle = math::to_axis_angle(quat);
        set_curve_sample(curves_rot[0], i, tf, axis_angle.angle().radian());
        set_curve_sample(curves_rot[1], i, tf, axis_angle.axis().x);
        set_curve_sample(curves_rot[2], i, tf, axis_angle.axis().y);
        set_curve_sample(curves_rot[3], i, tf, axis_angle.axis().z);
      } break;
      default: {
        math::EulerXYZ euler = math::to_euler(quat);
        set_curve_sample(curves_rot[0], i, tf, euler.x().radian());
        set_curve_sample(curves_rot[1], i, tf, euler.y().radian());
        set_curve_sample(curves_rot[2], i, tf, euler.z().radian());
      } break;
    }

    set_curve_sample(curves_scale[0], i, tf, float(xform.scale.x));
    set_curve_sample(curves_scale[1], i, tf, float(xform.scale.y));
    set_curve_sample(curves_scale[2], i, tf, float(xform.scale.z));
  }

  /* Finalize the curves. */
  for (FCurve *cu : curves_pos) {
    finalize_curve(action, anim, cu);
  }
  for (FCurve *cu : curves_rot) {
    finalize_curve(action, anim, cu);
  }
  for (FCurve *cu : curves_scale) {
    finalize_curve(action, anim, cu);
  }
}

static void create_blend_shape_curves(const ElementAnimations &anim,
                                      bAction *action,
                                      animrig::Channelbag &channelbag,
                                      const double fps,
                                      const float anim_offset)
{
  const ufbx_blend_channel *fchan = ufbx_as_blend_channel(anim.prop_blend_shape->element);
  BLI_assert(fchan != nullptr);
  std::string rna_path = std::string("key_blocks[\"") + fchan->target_shape->name.data +
                         "\"].value";
  const ufbx_anim_curve *input_curve = anim.prop_blend_shape->anim_value->curves[0];
  FCurve *curve = create_fcurve(channelbag, {rna_path, 0}, input_curve->keyframes.count);
  for (int i = 0; i < input_curve->keyframes.count; i++) {
    const ufbx_keyframe &fkey = input_curve->keyframes[i];
    double t = fkey.time;
    float tf = float(t * fps + anim_offset);
    float val = float(fkey.value / 100.0); /* FBX shape weights are 0..100 range. */
    set_curve_sample(curve, i, tf, val);
  }

  /* Finalize the curves. */
  finalize_curve(action, anim, curve);
}

void import_animations(Main &bmain,
                       const ufbx_scene &fbx,
                       const FbxElementMapping &mapping,
                       const double fps,
                       const float anim_offset)
{
  /* Note: mixing is completely ignored for now, each layer results in an independent set of
   * actions. */

  Vector<ElementAnimations> animations = gather_animated_properties(fbx, mapping);

  Map<std::string, bAction *> action_name_map;
  for (const ElementAnimations &anim : animations) {
    bAction *action = create_action(bmain, anim, action_name_map);
    animrig::Channelbag &channelbag = animrig::action_channelbag_ensure(*action, *anim.target_id);

    if (anim.prop_position || anim.prop_rotation || anim.prop_scale) {
      create_transform_curves(anim, action, channelbag, fps, anim_offset);
    }
    if (anim.prop_blend_shape) {
      create_blend_shape_curves(anim, action, channelbag, fps, anim_offset);
    }
  }
}

}  // namespace blender::io::fbx
