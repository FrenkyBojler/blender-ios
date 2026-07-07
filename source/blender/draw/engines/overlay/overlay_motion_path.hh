/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "overlay_base.hh"

namespace blender::draw::overlay {

/**
 * Display object and armature motion path.
 * Motion paths can be found in (Object > Motion Paths) or (Data > Motion Paths) for armatures.
 */
class MotionPath : Overlay {

 private:
  PassSimple motion_path_ps_ = {"motion_path_ps_"};

  PassSimple::Sub *line_ps_ = nullptr;
  PassSimple::Sub *vert_ps_ = nullptr;

 public:
  void begin_sync(Resources &res, const State &state) final;

  void object_sync(Manager & /*manager*/,
                   const ObjectRef &ob_ref,
                   Resources & /*res*/,
                   const State &state) final;

  void draw_color_only(Framebuffer &framebuffer, Manager &manager, View &view) final;

 private:
  void motion_path_sync(const State &state,
                        const Object *ob,
                        const bPoseChannel *pchan,
                        const bAnimVizSettings &avs,
                        bMotionPath *mpath);
};

}  // namespace blender::draw::overlay
