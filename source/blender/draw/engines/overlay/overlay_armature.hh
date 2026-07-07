/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "DRW_render.hh"

#include "overlay_base.hh"
#include "overlay_shader_shared.hh"

namespace blender::draw::overlay {
enum eArmatureDrawMode {
  ARM_DRAW_MODE_OBJECT,
  ARM_DRAW_MODE_POSE,
  ARM_DRAW_MODE_EDIT,
};

/**
 * Displays armature objects.
 * This includes Object, Edit and Pose mode.
 */
class Armatures : Overlay {
  using EmptyInstanceBuf = ShapeInstanceBuf<ExtraInstanceData>;
  using BoneInstanceBuf = ShapeInstanceBuf<BoneInstanceData>;
  using BoneEnvelopeBuf = ShapeInstanceBuf<BoneEnvelopeData>;
  using BoneStickBuf = ShapeInstanceBuf<BoneStickData>;
  using DegreesOfFreedomBuf = ShapeInstanceBuf<ExtraInstanceData>;

 private:
  const SelectionType selection_type_;

  PassSimple armature_ps_ = {"Armature"};

  /* Force transparent drawing in X-ray mode. */
  bool draw_transparent = false;
  /* Force disable drawing relation is relations are off in viewport. */
  bool show_relations = false;
  /* Show selection state. */
  bool show_outline = false;

  struct BoneBuffers {
    const SelectionType selection_type_;

    /* Bone end points (joints). */
    PassSimple::Sub *sphere_fill = nullptr;
    PassSimple::Sub *sphere_outline = nullptr;
    /* Bone shapes. */
    PassSimple::Sub *shape_fill = nullptr;
    PassSimple::Sub *shape_outline = nullptr;
    /* Custom bone wire-frame. */
    PassSimple::Sub *shape_wire = nullptr;
    PassSimple::Sub *shape_wire_strip = nullptr;
    /* Envelopes. */
    PassSimple::Sub *envelope_fill = nullptr;
    PassSimple::Sub *envelope_outline = nullptr;
    PassSimple::Sub *envelope_distance = nullptr;
    /* Stick bones. */
    PassSimple::Sub *stick = nullptr;
    /* Wire bones. */
    PassSimple::Sub *wire = nullptr;

    /* Bone axes. */
    PassSimple::Sub *arrows = nullptr;
    /* Degrees of freedom. */
    PassSimple::Sub *degrees_of_freedom_fill = nullptr;
    PassSimple::Sub *degrees_of_freedom_wire = nullptr;
    /* Relations. */
    PassSimple::Sub *relations = nullptr;

    BoneInstanceBuf bbones_fill_buf = {selection_type_, "bbones_fill_buf"};
    BoneInstanceBuf bbones_outline_buf = {selection_type_, "bbones_outline_buf"};

    BoneInstanceBuf octahedral_fill_buf = {selection_type_, "octahedral_fill_buf"};
    BoneInstanceBuf octahedral_outline_buf = {selection_type_, "octahedral_outline_buf"};

    BoneInstanceBuf sphere_fill_buf = {selection_type_, "sphere_fill_buf"};
    BoneInstanceBuf sphere_outline_buf = {selection_type_, "sphere_outline_buf"};

    BoneEnvelopeBuf envelope_fill_buf = {selection_type_, "envelope_fill_buf"};
    BoneEnvelopeBuf envelope_outline_buf = {selection_type_, "envelope_outline_buf"};
    BoneEnvelopeBuf envelope_distance_buf = {selection_type_, "envelope_distance_buf"};

    BoneStickBuf stick_buf = {selection_type_, "stick_buf"};

    LinePrimitiveBuf wire_buf = {selection_type_, "wire_buf"};

    EmptyInstanceBuf arrows_buf = {selection_type_, "arrows_buf"};

    DegreesOfFreedomBuf degrees_of_freedom_fill_buf = {SelectionType::DISABLED,
                                                       "degrees_of_freedom_buf"};
    DegreesOfFreedomBuf degrees_of_freedom_wire_buf = {SelectionType::DISABLED,
                                                       "degrees_of_freedom_buf"};

    LinePrimitiveBuf relations_buf = {SelectionType::DISABLED, "relations_buf"};

    Map<gpu::Batch *, std::unique_ptr<BoneInstanceBuf>> custom_shape_fill;
    Map<gpu::Batch *, std::unique_ptr<BoneInstanceBuf>> custom_shape_outline;
    Map<gpu::Batch *, std::unique_ptr<BoneInstanceBuf>> custom_shape_wire;
    Map<gpu::Batch *, std::unique_ptr<BoneInstanceBuf>> custom_shape_wire_strip;

    BoneInstanceBuf &custom_shape_fill_get_buffer(gpu::Batch *geom)
    {
      return *custom_shape_fill.lookup_or_add_cb(geom, [this]() {
        return std::make_unique<BoneInstanceBuf>(this->selection_type_, "CustomBoneSolid");
      });
    }

    BoneInstanceBuf &custom_shape_outline_get_buffer(gpu::Batch *geom)
    {
      return *custom_shape_outline.lookup_or_add_cb(geom, [this]() {
        return std::make_unique<BoneInstanceBuf>(this->selection_type_, "CustomBoneOutline");
      });
    }

    BoneInstanceBuf &custom_shape_wire_get_buffer(gpu::Batch *geom)
    {
      if (geom->prim_type == GPU_PRIM_LINE_STRIP) {
        return *custom_shape_wire_strip.lookup_or_add_cb(geom, [this]() {
          return std::make_unique<BoneInstanceBuf>(this->selection_type_, "CustomBoneWireStrip");
        });
      }
      return *custom_shape_wire.lookup_or_add_cb(geom, [this]() {
        return std::make_unique<BoneInstanceBuf>(this->selection_type_, "CustomBoneWire");
      });
    }

    BoneBuffers(const SelectionType selection_type) : selection_type_(selection_type) {};
  };

  BoneBuffers opaque_ = {selection_type_};
  BoneBuffers transparent_ = {selection_type_};

 public:
  Armatures(const SelectionType selection_type) : selection_type_(selection_type) {};

  void begin_sync(Resources &res, const State &state) final;

  struct DrawContext {
    /* Current armature object */
    Object *ob = nullptr;
    const ObjectRef *ob_ref = nullptr;
    bArmature *armature = nullptr;

    /* Note: can be mutated inside `draw_armature_pose()`. */
    eArmatureDrawMode draw_mode = ARM_DRAW_MODE_OBJECT;
    eArmature_Drawtype drawtype = ARM_DRAW_TYPE_OCTA;

    Armatures::BoneBuffers *bone_buf = nullptr;
    Resources *res = nullptr;
    DRWTextStore *dt = nullptr;

    /* Not a theme, this is an override. */
    const float *const_color = nullptr;
    /* Wire thickness. */
    float const_wire = 0.0f;

    bool do_relations = false;
    bool transparent = false;
    bool show_relations = false;
    bool draw_envelope_distance = false;
    bool draw_relation_from_head = false;
    bool show_text = false;
    /* Draw the inner part of the bones, otherwise render just outlines. */
    bool is_filled = false;

    const ThemeWireColor *bcolor = nullptr; /* Pose-channel color. */

    DrawContext() = default;
  };

  DrawContext create_draw_context(const ObjectRef &ob_ref,
                                  Resources &res,
                                  const State &state,
                                  eArmatureDrawMode draw_mode);

  void edit_object_sync(Manager & /*manager*/,
                        const ObjectRef &ob_ref,
                        Resources &res,
                        const State &state) final;

  void object_sync(Manager & /*manager*/,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final;

  void end_sync(Resources &res, const State & /*state*/) final;

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }

    GPU_framebuffer_bind(framebuffer);
    manager.submit(armature_ps_, view);
  }

  static bool is_pose_mode(const Object *armature_ob, const State &state);
};

}  // namespace blender::draw::overlay
