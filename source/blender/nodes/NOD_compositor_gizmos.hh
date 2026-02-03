/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

// todo(habib): doxygen doc

struct bContext;
struct wmGizmoGroup;
struct ARegion;
struct wmGizmoGroupType;

namespace blender::nodes::gizmos {

const SpaceNode *find_node_editor(const bContext *C);

bool WIDGETGROUP_node_box_mask_poll(const bContext *C, wmGizmoGroupType *gzgt);
void WIDGETGROUP_node_box_mask_setup(const bContext *C, wmGizmoGroup *gzgroup);
void WIDGETGROUP_bbox_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup);
void WIDGETGROUP_node_mask_refresh(const bContext *C, wmGizmoGroup *gzgroup);

}  // namespace blender::nodes::gizmos
